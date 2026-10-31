"""Independent loopback integration tests for the optional TCP gateway."""
import concurrent.futures
import pathlib
import queue
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest

BINARY = str(pathlib.Path(sys.argv.pop(1)).resolve())


class Server:
    def __init__(self, *options):
        self.process = subprocess.Popen(
            [BINARY, "--port", "0", *options], stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1,
        )
        self.lines = []
        self.ready = queue.Queue()
        self.stats = {}

        def drain():
            for line in self.process.stdout:
                self.lines.append(line.rstrip())
                self.ready.put(line.rstrip())
            self.ready.put(None)

        self.reader = threading.Thread(target=drain, daemon=True)
        self.reader.start()
        deadline = time.monotonic() + 20
        try:
            while True:
                line = self.ready.get(timeout=max(0.01, deadline - time.monotonic()))
                if line is None:
                    raise RuntimeError(f"server exited during startup: {self.lines}")
                if line.startswith("LISTEN "):
                    _, self.host, port = line.split()
                    self.port = int(port)
                    break
        except BaseException:
            self.process.kill()
            self.process.wait(timeout=5)
            self.reader.join(timeout=5)
            self.process.stdout.close()
            raise

    def connect(self):
        return Client(self.host, self.port)

    def stop(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
        try:
            code = self.process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
            raise AssertionError(f"shutdown hung: {self.lines}")
        self.reader.join(timeout=5)
        self.process.stdout.close()
        for line in self.lines:
            if line.startswith("STATS "):
                self.stats.update({k: int(v) for k, v in (p.split("=", 1) for p in line.split()[1:])})
        if code != 0:
            raise AssertionError(f"server exit {code}: {self.lines}")
        if not self.stats:
            raise AssertionError(f"missing shutdown accounting: {self.lines}")
        if self.stats["accepted"] != self.stats["processed"]:
            raise AssertionError(f"lost admitted requests: {self.stats}")
        return self.stats

    def __enter__(self):
        return self

    def __exit__(self, typ, value, tb):
        try:
            self.stop()
        except Exception as error:
            if typ is None:
                raise
            if hasattr(value, "add_note"):
                value.add_note(f"gateway cleanup: {error}")
            else:
                print(f"gateway cleanup: {error}", file=sys.stderr)


class Client:
    def __init__(self, host, port):
        self.socket = socket.create_connection((host, port), timeout=10)
        self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buffer = bytearray()
        self.next_id = 1

    def send(self, command, rid=None):
        if rid is None:
            rid = self.next_id
        self.next_id = max(self.next_id, rid + 1)
        self.socket.sendall(f"EC1 {rid} {command}\n".encode())
        return rid

    def receive(self, timeout=15):
        deadline = time.monotonic() + timeout
        records = []
        correlation = None
        while True:
            while b"\n" not in self.buffer:
                self.socket.settimeout(max(0.001, deadline - time.monotonic()))
                data = self.socket.recv(4096)
                if not data:
                    raise EOFError(f"EOF before END; records={records[-3:]}")
                self.buffer.extend(data)
                if len(self.buffer) > 1_000_000:
                    raise AssertionError("unbounded response frame")
            raw, _, rest = self.buffer.partition(b"\n")
            self.buffer = bytearray(rest)
            fields = raw.decode("ascii").split()
            if len(fields) < 4 or fields[0] != "EC1":
                raise AssertionError(f"bad response: {raw}")
            current = int(fields[1]), int(fields[2])
            if correlation is None:
                correlation = current
            if correlation != current:
                raise AssertionError(f"interleaved/wrong-client response: {current} vs {correlation}")
            if fields[3] == "END":
                return (*correlation, records)
            records.append(fields[3:])

    def request(self, command, rid=None):
        expected = self.send(command, rid)
        actual = self.receive()
        if actual[0] != expected:
            raise AssertionError(f"request ID {expected} routed to {actual}")
        return actual

    def close(self):
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


class NetworkTests(unittest.TestCase):
    def test_01_multi_client_matching_queries_and_cancellation(self):
        with Server() as server, server.connect() as seller, server.connect() as buyer:
            self.assertEqual(seller.request("PING")[2], [["PONG"]])
            sell = seller.request("LIMIT 1 SELL 10 24001", 101)
            self.assertEqual(sell[2], [["OK", "1", "0", "10", "0", "0"]])
            buy = buyer.request("LIMIT 2 BUY 4 24001", 201)
            self.assertGreater(buy[1], sell[1])
            self.assertEqual(buy[2], [["OK", "2", "4", "0", "0", "1"], ["TRADE", "1", "2", "24001", "4"]])
            self.assertEqual(seller.request("BEST")[2], [["BEST", "NONE", "24001", "1", "6"]])
            self.assertEqual(buyer.request("MARKET 3 BUY 2")[2][1], ["TRADE", "1", "3", "24001", "2"])
            self.assertEqual(seller.request("CANCEL 1")[2], [["OK", "1", "0", "0", "4", "0"]])
            self.assertEqual(buyer.request("BEST")[2], [["BEST", "NONE", "NONE", "0", "0"]])

    def test_02_fragmented_coalesced_crlf_and_maximum_valid_frame(self):
        with Server() as server, server.connect() as client:
            for fragment in (b"E", b"C1 1 LI", b"MIT 1 SEL", b"L 3 24001\r", b"\n"):
                client.socket.sendall(fragment)
            self.assertEqual(client.receive()[2][0], ["OK", "1", "0", "3", "0", "0"])
            client.socket.sendall(b"EC1 2 PING\nEC1 3 BEST\nEC1 4 MARKET 2 BUY 1\n")
            responses = [client.receive() for _ in range(3)]
            self.assertEqual([r[0] for r in responses], [2, 3, 4])
            self.assertEqual([r[1] for r in responses], sorted(r[1] for r in responses))
            frame = b"EC1 5 PING".ljust(4096, b" ") + b"\n"
            client.socket.sendall(frame)
            self.assertEqual(client.receive()[2], [["PONG"]])

    def test_03_malformed_version_numeric_control_and_oversize(self):
        with Server() as server:
            for frame in (b"EC2 1 PING\n", b"EC1 2 NOPE\n", b"EC1 3 LIMIT 1 BUY 0 1\n",
                          b"EC1 4 MARKET 1 SELL 18446744073709551616\n", b"EC1 5 PI\x00NG\n"):
                with server.connect() as client:
                    client.socket.sendall(frame)
                    response = client.receive()
                    self.assertEqual(response[1], 0)
                    self.assertEqual(response[2][0][0], "ERROR")
            with server.connect() as client:
                client.socket.sendall(b"EC1 1 PING " + b"x" * 4096 + b"\n")
                self.assertEqual(client.receive()[2][0], ["ERROR", "LINE_TOO_LARGE"])
            with server.connect() as client:
                self.assertEqual(client.request("PING")[2], [["PONG"]])

    def test_04_duplicates_never_execute_again(self):
        with Server() as server, server.connect() as client:
            client.socket.sendall(b"EC1 1 LIMIT 1 BUY 7 24000\nEC1 1 LIMIT 2 BUY 8 24000\n")
            self.assertEqual(client.receive()[2][0][0], "OK")
            self.assertEqual(client.receive()[2], [["ERROR", "DUPLICATE_REQUEST"]])
            self.assertEqual(client.request("BEST", 2)[2], [["BEST", "24000", "NONE", "1", "7"]])
            with server.connect() as retry:
                response = retry.request("LIMIT 1 BUY 7 24000")
                self.assertEqual(response[2][0][:2], ["REJECT", "DUPLICATE_ID"])
            self.assertEqual(client.request("CANCEL 1", 3)[2][0][0], "OK")
            self.assertEqual(client.request("LIMIT 1 BUY 7 24000", 4)[2][0][:2], ["REJECT", "DUPLICATE_ID"])

    def test_05_half_close_disconnect_reset_and_connection_reuse(self):
        with Server() as server:
            with server.connect() as client:
                client.send("LIMIT 1 SELL 2 24001")
                client.socket.shutdown(socket.SHUT_WR)
                self.assertEqual(client.receive()[2][0][0], "OK")
            with server.connect() as client:
                self.assertEqual(client.request("BEST")[2], [["BEST", "NONE", "24001", "1", "2"]])
            for _ in range(20):
                reset = server.connect()
                reset.send("PING", 999)
                reset.socket.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                reset.close()
                with server.connect() as fresh:
                    self.assertEqual(fresh.request("PING", 1)[2], [["PONG"]])

    def test_06_stream_large_execution_through_tiny_output(self):
        with Server("--max-output", "512", "--send-buffer", "1024", "--write-chunk", "7") as server, server.connect() as client:
            for n in range(1, 129):
                self.assertEqual(client.request(f"LIMIT {n} SELL 1 {24000+n}")[2][0][0], "OK")
            reply = client.request("MARKET 1000 BUY 128")
            self.assertEqual(reply[2][0], ["OK", "1000", "128", "0", "0", "128"])
            self.assertEqual(len(reply[2]), 129)
            self.assertEqual([int(t[1]) for t in reply[2][1:]], list(range(1, 129)))
            self.assertEqual(client.request("BEST")[2], [["BEST", "NONE", "NONE", "0", "0"]])
        self.assertLessEqual(server.stats["output_high_water"], 512)
        self.assertGreater(server.stats["partial_writes"], 0)

    def test_07_queue_saturation_is_explicit_and_accounted(self):
        with Server("--queue-capacity", "1", "--completion-capacity", "1", "--pending-per-client", "256") as server, server.connect() as client:
            for n in range(1, 1025):
                client.request(f"LIMIT {n} SELL 1 24001")
            commands = ["MARKET 5000 BUY 1024"] + ["PING"] * 4095
            first = client.next_id
            client.socket.sendall("".join(f"EC1 {first+i} {c}\n" for i, c in enumerate(commands)).encode())
            overload = 0
            for i in range(len(commands)):
                rid, seq, records = client.receive(timeout=30)
                self.assertEqual(rid, first + i)
                if records[0] == ["ERROR", "OVERLOADED"]:
                    self.assertEqual(seq, 0)
                    overload += 1
                else:
                    self.assertGreater(seq, 0)
            self.assertGreater(overload, 0, "burst failed to exercise saturation")
        self.assertGreater(server.stats["overloaded"], 0)
        self.assertEqual(server.stats["queue_high_water"], 1)

    def test_08_slow_reader_is_bounded_and_other_client_progresses(self):
        with Server("--max-output", "512", "--send-buffer", "1024", "--pending-per-client", "4") as server:
            slow = server.connect()
            try:
                slow.socket.setblocking(False)
                payload = "".join(f"EC1 {n} PING\n" for n in range(1, 50001)).encode()
                sent = 0
                while sent < len(payload):
                    try:
                        amount = slow.socket.send(payload[sent:])
                        if not amount:
                            break
                        sent += amount
                    except BlockingIOError:
                        break
                self.assertGreater(sent, 0)
                with server.connect() as other:
                    for _ in range(30):
                        self.assertEqual(other.request("PING")[2], [["PONG"]])
            finally:
                slow.socket.close()
        self.assertLessEqual(server.stats["output_high_water"], 512)

    def test_09_concurrent_fixed_workload_routing_and_sequence(self):
        with Server("--queue-capacity", "64") as server:
            barrier = threading.Barrier(8)

            def work(index):
                sequences = []
                with server.connect() as client:
                    barrier.wait(timeout=15)
                    for n in range(60):
                        order_id = index * 10000 + n + 1
                        side = "BUY" if index % 2 else "SELL"
                        price = 23000 if side == "BUY" else 25000
                        for command in (f"LIMIT {order_id} {side} 3 {price}", f"CANCEL {order_id}"):
                            rid, seq, records = client.request(command)
                            self.assertEqual(records[0][0], "OK")
                            sequences.append(seq)
                self.assertEqual(sequences, sorted(sequences))
                return sequences

            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                all_sequences = list(pool.map(work, range(8)))
            flat = [s for row in all_sequences for s in row]
            self.assertEqual(len(set(flat)), 960)
            with server.connect() as final:
                self.assertEqual(final.request("BEST")[2], [["BEST", "NONE", "NONE", "0", "0"]])
        self.assertEqual(server.stats["accepted"], 961)

    def test_10_shutdown_drains_admitted_work_and_closes_threads(self):
        server = Server("--queue-capacity", "8", "--pending-per-client", "64", "--shutdown-grace-ms", "100")
        client = server.connect()
        try:
            client.socket.sendall("".join(f"EC1 {n} PING\n" for n in range(1, 2001)).encode())
            self.assertEqual(client.receive()[2][0], ["PONG"])
            stats = server.stop()
            self.assertGreater(stats["accepted"], 0)
            self.assertEqual(stats["accepted"], stats["processed"])
        finally:
            client.close()
            if server.process.poll() is None:
                server.stop()

    def test_11_startup_configuration_and_bind_failure(self):
        for args in (["--port", "65536"], ["--queue-capacity", "0"], ["--max-output", "0"], ["--host", "garbage"]):
            result = subprocess.run([BINARY, *args], capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0, args)
            self.assertTrue(result.stderr or result.stdout, args)
        with Server() as server:
            result = subprocess.run([BINARY, "--port", str(server.port)], capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)

    def test_12_recorded_processing_order_replays_identically(self):
        with Server() as server:
            barrier = threading.Barrier(4)

            def record(index):
                log = []
                with server.connect() as client:
                    barrier.wait(timeout=15)
                    for n in range(12):
                        oid = index * 1000 + n * 2 + 1
                        side = "BUY" if index % 2 else "SELL"
                        opposite = "SELL" if side == "BUY" else "BUY"
                        for command in (f"LIMIT {oid} {side} 3 24000",
                                        f"MARKET {oid+1} {opposite} 1", f"CANCEL {oid}", "BEST"):
                            _, seq, records = client.request(command)
                            self.assertGreater(seq, 0)
                            log.append((seq, command, records))
                return log

            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                logs = list(pool.map(record, range(4)))
        ordered = sorted(entry for log in logs for entry in log)
        self.assertEqual([entry[0] for entry in ordered], list(range(1, 193)))
        with Server() as replay, replay.connect() as client:
            for seq, command, expected in ordered:
                actual = client.request(command)
                self.assertEqual(actual[1], seq)
                self.assertEqual(actual[2], expected, f"recorded sequence {seq}: {command}")

    def test_13_incomplete_frame_and_connection_capacity(self):
        with Server("--max-clients", "2") as server, server.connect() as first, server.connect() as second:
            self.assertEqual(first.request("PING")[2], [["PONG"]])
            self.assertEqual(second.request("PING")[2], [["PONG"]])
            with server.connect() as excess:
                with self.assertRaises((EOFError, OSError)):
                    excess.request("PING")
            first.socket.sendall(b"EC1 2 PING")
            first.socket.shutdown(socket.SHUT_WR)
            self.assertEqual(first.receive()[2], [["ERROR", "INCOMPLETE_FRAME"]])
            self.assertEqual(second.request("PING")[2], [["PONG"]])


if __name__ == "__main__":
    unittest.main(verbosity=2)
