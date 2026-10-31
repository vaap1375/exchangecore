#!/usr/bin/env python3
"""Loopback EC1 request throughput and separate instrumented request RTT trials.

Uses fresh gateways for every trial, bounded client pipelines, and no load retries.
Results include Python, socket, scheduling and response parsing costs. They are
not matching-engine-only measurements. Run --help for reproducible configuration.
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass, field
import math
import os
from pathlib import Path
import platform
import random
import statistics
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from network_common import FrameClient, ManagedServer, ProtocolError, Response, child_peak_rss_kib


@dataclass(frozen=True)
class Operation:
    command: str
    kind: str
    order_id: int = 0
    quantity: int = 0


@dataclass
class WorkerResult:
    completed: int = 0
    processed: int = 0
    overloaded: int = 0
    rejected: int = 0
    trades: int = 0
    filled: int = 0
    canceled: int = 0
    warmup_processed: int = 0
    warmup_overloaded: int = 0
    sent_bytes: int = 0
    received_bytes: int = 0
    finished_ns: int = 0
    sequences: set[int] = field(default_factory=set)
    latencies_ns: list[int] = field(default_factory=list)
    mismatches: list[str] = field(default_factory=list)
    error: str = ""


@dataclass
class Trial:
    elapsed_seconds: float
    workers: list[WorkerResult]
    stats: dict[str, int]
    child_peak_rss_kib: int

    def total(self, field_name: str) -> int:
        return sum(getattr(worker, field_name) for worker in self.workers)

    @property
    def completed_rate(self) -> float:
        return self.total("completed") / self.elapsed_seconds

    @property
    def processed_rate(self) -> float:
        return self.total("processed") / self.elapsed_seconds


def make_operations(workload: str, count: int, seed: int, client_index: int) -> list[Operation]:
    generator = random.Random(seed + client_index * 0x9E3779B9)
    operations: list[Operation] = []
    for index in range(count):
        order_id = ((client_index + 1) << 48) | (index + 1)
        if workload == "ping":
            operations.append(Operation("PING", "PING"))
        elif index % 2 == 0:
            quantity = generator.randrange(1, 11)
            side, price = ("SELL", 24000) if workload == "matching" else ("BUY", 23999)
            operations.append(Operation(f"LIMIT {order_id} {side} {quantity} {price}",
                                        "LIMIT", order_id, quantity))
        elif workload == "matching":
            quantity = operations[-1].quantity
            operations.append(Operation(f"MARKET {order_id} BUY {quantity}", "MARKET", order_id, quantity))
        else:
            previous = operations[-1]
            operations.append(Operation(f"CANCEL {previous.order_id}", "CANCEL", previous.order_id,
                                        previous.quantity))
    return operations


def record_response(result: WorkerResult, response: Response, operation: Operation) -> None:
    result.completed += 1
    result.received_bytes += response.wire_bytes
    record = response.records[0]
    if response.kind == "ERROR":
        if response.sequence != 0 or record != ("ERROR", "OVERLOADED") or len(response.records) != 1:
            raise ProtocolError(f"unexpected transport error {response.lines()}")
        result.overloaded += 1
        return
    if response.sequence == 0 or response.sequence in result.sequences:
        raise ProtocolError("missing or repeated processing sequence")
    result.sequences.add(response.sequence)
    result.processed += 1
    mismatch = ""
    if response.kind == "REJECT":
        if (len(record) != 4 or len(response.records) != 1 or int(record[2]) != operation.order_id or
                int(record[3]) != (0 if operation.kind == "CANCEL" else operation.quantity)):
            raise ProtocolError("rejection order ID or quantity correlation mismatch")
        result.rejected += 1
        mismatch = f"unexpected engine rejection: {record}"
    elif operation.kind == "PING":
        if response.records != (("PONG",),):
            raise ProtocolError(f"unexpected ping response {response.lines()}")
    else:
        if response.kind != "OK" or len(record) != 6:
            raise ProtocolError(f"unexpected order response {response.lines()}")
        oid, filled, remaining, canceled, trades = map(int, record[1:])
        trade_records = response.records[1:]
        if oid != operation.order_id or trades != len(trade_records):
            raise ProtocolError("order ID or trade-count correlation mismatch")
        trade_quantity = 0
        for trade in trade_records:
            if len(trade) != 5 or trade[0] != "TRADE":
                raise ProtocolError("invalid execution record")
            maker, taker, price, quantity = map(int, trade[1:])
            if maker <= 0 or taker != oid or price != 24000 or quantity <= 0:
                raise ProtocolError("invalid execution identifiers, price or quantity")
            trade_quantity += quantity
        if min(filled, remaining, canceled, trades) < 0 or trade_quantity != filled:
            raise ProtocolError("negative or inconsistent quantity accounting")
        result.trades += trades
        result.filled += filled
        result.canceled += canceled
        if operation.kind == "LIMIT" and (filled, remaining, canceled) != (0, operation.quantity, 0):
            mismatch = f"resting limit accounting mismatch: {record}"
        elif operation.kind == "MARKET" and (filled, remaining, canceled) != (operation.quantity, 0, 0):
            mismatch = f"market accounting mismatch: {record}"
        elif operation.kind == "CANCEL" and (filled, remaining, canceled) != (0, 0, operation.quantity):
            mismatch = f"cancel accounting mismatch: {record}"
    if mismatch and len(result.mismatches) < 3:
        # If a preceding order was overloaded, dependent markets/cancels may
        # legitimately differ. The caller rejects these only in overload-free runs.
        result.mismatches.append(mismatch)


def worker_run(host: str, port: int, operations: list[Operation], args: argparse.Namespace,
               barrier: threading.Barrier, begin: threading.Event, result: WorkerResult,
               sample_indices: set[int] | None) -> None:
    try:
        with FrameClient(host, port, timeout=args.timeout) as client:
            for rid in range(1, args.warmup_requests + 1):
                response = client.request("PING", rid)
                if response.sequence > 0 and response.records == (("PONG",),):
                    result.warmup_processed += 1
                elif response.sequence == 0 and response.records == (("ERROR", "OVERLOADED"),):
                    result.warmup_overloaded += 1
                else:
                    raise ProtocolError(f"unexpected warmup response {response.lines()}")
            barrier.wait(timeout=args.timeout)
            if not begin.wait(timeout=args.timeout):
                raise TimeoutError("benchmark start barrier timed out")
            next_index = 0
            pending: dict[int, tuple[Operation, int]] = {}
            while next_index < len(operations) or pending:
                while next_index < len(operations) and len(pending) < args.pipeline:
                    rid = args.warmup_requests + next_index + 1
                    operation = operations[next_index]
                    start = time.perf_counter_ns() if sample_indices is not None and next_index in sample_indices else 0
                    result.sent_bytes += client.send(rid, operation.command)
                    pending[rid] = operation, start
                    next_index += 1
                response = client.receive()
                if response.request_id not in pending:
                    raise ProtocolError(f"unexpected or duplicate response request ID {response.request_id}")
                operation, start = pending.pop(response.request_id)
                if start:
                    result.latencies_ns.append(time.perf_counter_ns() - start)
                record_response(result, response, operation)
            result.finished_ns = time.perf_counter_ns()
    except Exception as error:
        result.error = f"{type(error).__name__}: {error}"
        barrier.abort()
        result.finished_ns = time.perf_counter_ns()


def run_trial(operations: list[list[Operation]], args: argparse.Namespace, *, latency: bool = False) -> Trial:
    clients = len(operations)
    options = ["--queue-capacity", str(args.queue_capacity), "--completion-capacity", str(args.completion_capacity),
               "--pending-per-client", str(args.pending_per_client), "--max-clients", str(max(128, clients + 2)),
               "--max-orders", str(max(1_000_000, clients * args.requests_per_client))]
    sample_sets: list[set[int] | None] = [None] * clients
    if latency:
        sample_count = min(args.latency_samples, clients * args.requests_per_client)
        for index in range(clients):
            count = sample_count // clients + int(index < sample_count % clients)
            sample_sets[index] = set(random.Random(args.seed ^ index ^ 0x525454).sample(
                range(args.requests_per_client), count))
    results = [WorkerResult() for _ in range(clients)]
    barrier = threading.Barrier(clients + 1)
    begin = threading.Event()
    threads: list[threading.Thread] = []
    with ManagedServer(args.binary, options, timeout=max(10.0, args.timeout)) as server:
        for index in range(clients):
            thread = threading.Thread(target=worker_run,
                                      args=(server.host, server.port, operations[index], args, barrier, begin,
                                            results[index], sample_sets[index]), daemon=True)
            thread.start()
            threads.append(thread)
        try:
            barrier.wait(timeout=max(10.0, args.timeout))
            start_ns = time.perf_counter_ns()
            begin.set()
            deadline = time.monotonic() + args.trial_timeout
            for thread in threads:
                thread.join(timeout=max(0, deadline - time.monotonic()))
            if any(thread.is_alive() for thread in threads):
                raise TimeoutError("network trial exceeded global deadline")
        except threading.BrokenBarrierError as error:
            begin.set()
            details = "; ".join(result.error for result in results if result.error)
            raise RuntimeError(f"client startup failed: {details}") from error
        errors = [result.error for result in results if result.error]
        if errors:
            raise RuntimeError("; ".join(errors))
        end_ns = max(result.finished_ns for result in results)
        if any(result.completed != args.requests_per_client for result in results):
            raise RuntimeError("trial lost a response")
        seen: set[int] = set()
        for result in results:
            if seen.intersection(result.sequences):
                raise RuntimeError("processing sequence was reused across connections")
            seen.update(result.sequences)
        overloaded = sum(result.overloaded for result in results)
        if not overloaded:
            mismatches = [message for result in results for message in result.mismatches]
            if mismatches:
                raise RuntimeError("; ".join(mismatches[:3]))
        with FrameClient(server.host, server.port, timeout=args.timeout) as query:
            final = query.request("BEST")
            if final.sequence == 0 or final.kind != "BEST" or len(final.records[0]) != 5:
                raise ProtocolError("invalid final book query")
            if not overloaded and final.records != (("BEST", "NONE", "NONE", "0", "0"),):
                raise RuntimeError(f"overload-free workload left unexpected book state: {final.records}")
    required = {"accepted", "processed", "overloaded", "queue_high_water", "discarded"}
    if not required.issubset(server.stats):
        raise RuntimeError(f"gateway did not report required shutdown statistics: {server.stdout}")
    expected_processed = sum(result.processed + result.warmup_processed for result in results) + 1
    expected_overloaded = sum(result.overloaded + result.warmup_overloaded for result in results)
    if (server.stats["accepted"] != expected_processed or server.stats["processed"] != expected_processed or
            server.stats["overloaded"] != expected_overloaded or server.stats["discarded"] != 0):
        raise RuntimeError(f"server admission/completion accounting mismatch: {server.stats}")
    if (server.stats["queue_high_water"] > args.queue_capacity or
            server.stats.get("completion_high_water", 0) > args.completion_capacity):
        raise RuntimeError("server exceeded its configured queue bounds")
    if server.stats.get("parse_errors", 0):
        raise RuntimeError("gateway reported a parse error for generated valid requests")
    return Trial((end_ns - start_ns) / 1e9, results, server.stats.copy(), child_peak_rss_kib())


def nearest_rank(values: list[int], proportion: float) -> float:
    return values[max(1, math.ceil(len(values) * proportion)) - 1] / 1000


def summarize(workload: str, clients: int, trials: list[Trial], latency: Trial,
              args: argparse.Namespace) -> dict[str, object]:
    rates = [trial.completed_rate for trial in trials]
    latencies = sorted(value for result in latency.workers for value in result.latencies_ns)
    if not latencies:
        raise RuntimeError("latency replay recorded no samples")
    return {
        "workload": workload, "seed": args.seed, "clients": clients,
        "requests_per_client": args.requests_per_client, "pipeline_per_client": args.pipeline,
        "trials": args.trials, "warmup_requests_per_client": args.warmup_requests,
        "queue_capacity": args.queue_capacity, "completion_capacity": args.completion_capacity,
        "pending_per_client": args.pending_per_client,
        "completed_req_s_min": min(rates), "completed_req_s_median": statistics.median(rates),
        "completed_req_s_max": max(rates), "throughput_cv_percent": 100 * statistics.pstdev(rates) / statistics.mean(rates),
        "processed_req_s_median": statistics.median(trial.processed_rate for trial in trials),
        "latency_samples": len(latencies), "latency_avg_us": statistics.mean(latencies) / 1000,
        "latency_p50_us": nearest_rank(latencies, 0.50), "latency_p95_us": nearest_rank(latencies, 0.95),
        "latency_p99_us": nearest_rank(latencies, 0.99),
        "completed_total": sum(trial.total("completed") for trial in trials),
        "processed_total": sum(trial.total("processed") for trial in trials),
        "overloaded_total": sum(trial.total("overloaded") for trial in trials),
        "engine_rejected_total": sum(trial.total("rejected") for trial in trials),
        "trades_total": sum(trial.total("trades") for trial in trials),
        "filled_quantity_total": sum(trial.total("filled") for trial in trials),
        "canceled_quantity_total": sum(trial.total("canceled") for trial in trials),
        "request_bytes_total": sum(trial.total("sent_bytes") for trial in trials),
        "response_bytes_total": sum(trial.total("received_bytes") for trial in trials),
        "server_queue_high_water_max": max(trial.stats["queue_high_water"] for trial in trials),
        "server_completion_high_water_max": max(trial.stats.get("completion_high_water", 0) for trial in trials),
        "server_output_high_water_max": max(trial.stats.get("output_high_water", 0) for trial in trials),
        "server_discarded_total": sum(trial.stats["discarded"] for trial in trials),
        "server_rss_available": int(all("startup_rss_kib" in trial.stats and "peak_rss_kib" in trial.stats
                                        for trial in trials)),
        "server_startup_rss_kib_min": min(trial.stats.get("startup_rss_kib", 0) for trial in trials),
        "server_startup_rss_kib_max": max(trial.stats.get("startup_rss_kib", 0) for trial in trials),
        "server_peak_rss_kib_min": min(trial.stats.get("peak_rss_kib", 0) for trial in trials),
        "server_peak_rss_kib_max": max(trial.stats.get("peak_rss_kib", 0) for trial in trials),
        "server_high_water_growth_kib_max": max(max(0, trial.stats.get("peak_rss_kib", 0) -
                                                      trial.stats.get("startup_rss_kib", 0)) for trial in trials),
        "latency_overloaded": latency.total("overloaded"),
        "latency_server_peak_rss_kib": latency.stats.get("peak_rss_kib", 0),
        "children_lifetime_peak_rss_kib": latency.child_peak_rss_kib,
        "host": "127.0.0.1", "hardware_threads": os.cpu_count(), "hardware": args.hardware_label,
        "os": platform.platform(), "compiler": args.compiler_label, "build": args.build_label,
        "python": platform.python_version(), "gateway_binary": str(Path(args.binary).resolve().relative_to(ROOT))
        if Path(args.binary).resolve().is_relative_to(ROOT) else Path(args.binary).name,
    }


def csv_list(value: str) -> list[str]:
    fields = value.split(",")
    if not fields or any(not field for field in fields):
        raise argparse.ArgumentTypeError("expected a comma-separated nonempty list")
    return fields


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, epilog=(
        "No measured load request is retried, including OVERLOADED responses. Completed rates include overload "
        "responses; processed rates count only seq>0 responses. Dependent operations after overload may reject "
        "or remain unfilled. Latency uses a separate fresh-server replay and includes client/socket/parser costs. "
        "Server RSS fields are per-server process high-water marks, including startup/teardown, not live heap. "
        "Growth means peak minus startup high-water, not retained allocations. Child RSS is a separate lifetime "
        "high-water across reaped child processes."))
    parser.add_argument("--binary", "--server", dest="binary", type=Path,
                        default=ROOT / "build/release/exchangecore_gateway")
    parser.add_argument("--clients", type=csv_list, default=["1", "4", "8", "16"])
    parser.add_argument("--workloads", type=csv_list, default=["ping", "matching", "cancellation"])
    parser.add_argument("--requests-per-client", type=int, default=2000)
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--pipeline", type=int, default=8)
    parser.add_argument("--warmup-requests", type=int, default=2)
    parser.add_argument("--latency-samples", type=int, default=10000)
    parser.add_argument("--seed", type=int, default=60448)
    parser.add_argument("--queue-capacity", type=int, default=1024)
    parser.add_argument("--completion-capacity", type=int, default=1024)
    parser.add_argument("--pending-per-client", type=int, default=16)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--trial-timeout", type=float, default=60.0)
    parser.add_argument("--hardware-label", default="unrecorded")
    parser.add_argument("--compiler-label", default="unrecorded")
    parser.add_argument("--build-label", default="unverified")
    parser.add_argument("--csv", action="store_true")
    args = parser.parse_args()
    try:
        args.clients = [int(value) for value in args.clients]
    except ValueError:
        parser.error("--clients must contain integers")
    if len(set(args.clients)) != len(args.clients) or any(not 1 <= count <= 128 for count in args.clients):
        parser.error("client counts must be unique integers in 1..128")
    if any(workload not in {"ping", "matching", "cancellation"} for workload in args.workloads):
        parser.error("workloads must be ping,matching,cancellation")
    if not 2 <= args.requests_per_client <= 100000 or args.requests_per_client % 2:
        parser.error("requests per client must be even and in 2..100000")
    if max(args.clients) * args.requests_per_client > 1_000_000:
        parser.error("at most 1000000 requests per trial are supported")
    bounds = ((args.trials, 1, 20), (args.pipeline, 1, 1024), (args.warmup_requests, 0, 1000),
              (args.latency_samples, 1, 1_000_000), (args.queue_capacity, 1, 65536),
              (args.completion_capacity, 1, 65536), (args.pending_per_client, 1, 1024))
    if any(not low <= value <= high for value, low, high in bounds):
        parser.error("numeric option outside supported bounds")
    if not 0 <= args.seed < 1 << 64 or not all(math.isfinite(value) and value > 0
                                              for value in (args.timeout, args.trial_timeout)):
        parser.error("seed must be uint64 and timeouts finite and positive")
    return args


def main() -> int:
    args = arguments()
    writer: csv.DictWriter[str] | None = None
    try:
        if not args.csv:
            print("EC1 loopback benchmark: completed input requests/s; includes Python client/socket/parser costs.")
            print("Latency is a separate replay; no load retries. Child RSS is a process-lifetime aggregate.")
        for workload in args.workloads:
            for clients in args.clients:
                operations = [make_operations(workload, args.requests_per_client, args.seed, index)
                              for index in range(clients)]
                trials = [run_trial(operations, args) for _ in range(args.trials)]
                latency = run_trial(operations, args, latency=True)
                row = summarize(workload, clients, trials, latency, args)
                if args.csv:
                    if writer is None:
                        writer = csv.DictWriter(sys.stdout, fieldnames=list(row))
                        writer.writeheader()
                    writer.writerow(row)
                    sys.stdout.flush()
                else:
                    print(f"{workload:12s} clients={clients:2d} pipeline={args.pipeline:3d} "
                          f"completed/s min={row['completed_req_s_min']:.1f} "
                          f"median={row['completed_req_s_median']:.1f} max={row['completed_req_s_max']:.1f} "
                          f"processed/s={row['processed_req_s_median']:.1f} "
                          f"CV={row['throughput_cv_percent']:.2f}% "
                          f"RTT us avg={row['latency_avg_us']:.1f} p50={row['latency_p50_us']:.1f} "
                          f"p95={row['latency_p95_us']:.1f} p99={row['latency_p99_us']:.1f} "
                          f"overloaded={row['overloaded_total']} queue_high={row['server_queue_high_water_max']}")
        return 0
    except (OSError, RuntimeError, ValueError) as error:
        print(f"network benchmark: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("network benchmark interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
