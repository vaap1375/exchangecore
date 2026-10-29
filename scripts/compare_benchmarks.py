#!/usr/bin/env python3
"""Compare compatible CSV measurements; fail on changed workloads or results."""
import csv
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: compare_benchmarks.py BASELINE.csv RESERVED.csv")


def read(path):
    with open(path, newline="") as stream:
        rows = list(csv.DictReader(stream))
    return {row["workload"]: row for row in rows}


baseline, reserved = map(read, sys.argv[1:])
if baseline.keys() != reserved.keys() or not baseline:
    raise SystemExit("workload sets differ or are empty")
print("workload,baseline_ops_s,reserved_ops_s,change_percent,baseline_cv_percent,reserved_cv_percent,baseline_process_allocations,reserved_process_allocations")
for name, before in baseline.items():
    after = reserved[name]
    for field in ("seed", "events", "trials", "warmup", "latency_samples", "replay_checksum", "state_checksum", "build", "compiler", "platform"):
        if before[field] != after[field]:
            raise SystemExit(f"{name}: incompatible {field}")
    rate_a = float(before["throughput_median_ops_s"])
    rate_b = float(after["throughput_median_ops_s"])
    print(f"{name},{rate_a:.3f},{rate_b:.3f},{100 * (rate_b / rate_a - 1):.2f},"
          f"{before['throughput_cv_percent']},{after['throughput_cv_percent']},"
          f"{before['process_allocation_calls']},{after['process_allocation_calls']}")
