"""Bounded EC1 framing and managed local gateway processes; standard library only."""

from __future__ import annotations

from dataclasses import dataclass
import math
from pathlib import Path
import resource
import signal
import socket
import subprocess
import sys
import threading
import time
from typing import Sequence


class ProtocolError(RuntimeError):
    """The peer closed early or violated an EC1 response boundary."""


@dataclass(frozen=True)
class Response:
    request_id: int
    sequence: int
    records: tuple[tuple[str, ...], ...]
    wire_bytes: int

    @property
    def kind(self) -> str:
        return self.records[0][0]

    def lines(self) -> list[str]:
        prefix = f"EC1 {self.request_id} {self.sequence} "
        return [prefix + " ".join(record) for record in self.records] + [prefix + "END"]


class FrameClient:
    """One connection, one thread. max_line counts bytes excluding the trailing LF."""

    def __init__(self, host: str = "127.0.0.1", port: int = 9000, *, timeout: float = 5.0,
                 max_line: int = 4096, max_reply_bytes: int = 1 << 20):
        if not math.isfinite(timeout) or timeout <= 0 or max_line < 32 or max_reply_bytes < max_line:
            raise ValueError("invalid client timeout or framing bounds")
        self.timeout = timeout
        self.max_line = max_line
        self.max_reply_bytes = max_reply_bytes
        self.socket = socket.create_connection((host, port), timeout=timeout)
        try:
            self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except BaseException:
            self.socket.close()
            raise
        self._buffer = bytearray()
        self._next_id = 1

    def __enter__(self) -> FrameClient:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def close(self) -> None:
        self.socket.close()

    def _remaining(self, deadline: float) -> float:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("EC1 request deadline expired")
        return remaining

    def send_raw(self, line: str, *, deadline: float | None = None) -> int:
        if len(line) > self.max_line:
            raise ValueError(f"request exceeds client line bound {self.max_line}")
        if "\n" in line or "\r" in line:
            raise ValueError("a raw request must contain exactly one line")
        encoded = line.encode("ascii") + b"\n"
        limit = time.monotonic() + self.timeout if deadline is None else deadline
        self.socket.settimeout(self._remaining(limit))
        self.socket.sendall(encoded)
        return len(encoded)

    def send(self, request_id: int, command: str, *, deadline: float | None = None) -> int:
        if not 1 <= request_id < 1 << 64:
            raise ValueError("request ID must be a positive uint64")
        prefix = f"EC1 {request_id} "
        if len(prefix) + len(command) > self.max_line:
            raise ValueError(f"request exceeds client line bound {self.max_line}")
        return self.send_raw(prefix + command, deadline=deadline)

    def _line(self, deadline: float) -> bytes:
        while True:
            newline = self._buffer.find(b"\n")
            if newline >= 0:
                if newline > self.max_line:
                    raise ProtocolError("response line exceeds configured bound")
                line = bytes(self._buffer[:newline + 1])
                del self._buffer[:newline + 1]
                return line
            if len(self._buffer) > self.max_line:
                raise ProtocolError("unterminated response line exceeds configured bound")
            self.socket.settimeout(self._remaining(deadline))
            chunk = self.socket.recv(65536)
            if not chunk:
                raise ProtocolError("connection closed before response END")
            self._buffer.extend(chunk)

    def receive(self, *, deadline: float | None = None) -> Response:
        limit = time.monotonic() + self.timeout if deadline is None else deadline
        request_id: int | None = None
        sequence: int | None = None
        records: list[tuple[str, ...]] = []
        size = 0
        while True:
            raw = self._line(limit)
            size += len(raw)
            if size > self.max_reply_bytes:
                raise ProtocolError("response exceeds configured byte bound")
            try:
                fields = raw.decode("ascii").strip().split()
                if len(fields) < 4 or fields[0] != "EC1":
                    raise ValueError("invalid EC1 response prefix")
                rid, seq = int(fields[1]), int(fields[2])
                if not 0 <= rid < 1 << 64 or not 0 <= seq < 1 << 64:
                    raise ValueError("response identifiers exceed uint64")
            except (UnicodeError, ValueError) as error:
                raise ProtocolError(f"invalid EC1 response: {raw[:160]!r}") from error
            if request_id is None:
                request_id, sequence = rid, seq
            elif (rid, seq) != (request_id, sequence):
                raise ProtocolError("interleaved request or sequence before END")
            record = tuple(fields[3:])
            if record == ("END",):
                if not records:
                    raise ProtocolError("response contains END without a result")
                return Response(request_id, sequence, tuple(records), size)
            records.append(record)

    def request(self, command: str, request_id: int | None = None) -> Response:
        rid = self._next_id if request_id is None else request_id
        self._next_id = max(self._next_id, rid + 1)
        deadline = time.monotonic() + self.timeout
        self.send(rid, command, deadline=deadline)
        response = self.receive(deadline=deadline)
        if response.request_id != rid:
            raise ProtocolError(f"response routed to {response.request_id}, expected {rid}")
        return response


def child_peak_rss_kib() -> int:
    """Lifetime high-water across reaped children, not one gateway's retained RSS."""
    value = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    return int(value / 1024) if sys.platform == "darwin" else int(value)


class ManagedServer:
    """Start an ephemeral loopback gateway; continuously drain bounded diagnostics."""

    def __init__(self, binary: str | Path, options: Sequence[str] = (), *, timeout: float = 10.0):
        self.binary = Path(binary).resolve()
        self.options = list(options)
        self.timeout = timeout
        self.process: subprocess.Popen[str] | None = None
        self.host = "127.0.0.1"
        self.port = 0
        self.stats: dict[str, int] = {}
        self.stdout = ""
        self.stderr = ""
        self._ready = threading.Event()
        self._threads: list[threading.Thread] = []

    def _drain(self, stream: object, label: str) -> None:
        retained = 0
        while True:
            line = stream.readline(8193)  # type: ignore[attr-defined]
            if not line:
                break
            if retained < 65536:
                fragment = line[:65536 - retained]
                setattr(self, label, getattr(self, label) + fragment)
                retained += len(fragment)
            if label == "stdout":
                parts = line.split()
                if len(parts) == 3 and parts[0] == "LISTEN":
                    try:
                        port = int(parts[2])
                        if 1 <= port <= 65535 and parts[1] == "127.0.0.1":
                            self.host, self.port = parts[1], port
                            self._ready.set()
                    except ValueError:
                        pass
                elif parts and parts[0] == "STATS":
                    for part in parts[1:]:
                        key, separator, value = part.partition("=")
                        if separator and value.isdecimal():
                            self.stats[key] = int(value)

    def __enter__(self) -> ManagedServer:
        if not self.binary.is_file():
            raise FileNotFoundError(f"gateway not built: {self.binary}")
        self.process = subprocess.Popen(
            [str(self.binary), "--host", self.host, "--port", "0", *self.options],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1,
        )
        for label in ("stdout", "stderr"):
            thread = threading.Thread(target=self._drain, args=(getattr(self.process, label), label),
                                      daemon=True)
            thread.start()
            self._threads.append(thread)
        deadline = time.monotonic() + self.timeout
        while not self._ready.wait(min(0.05, max(0, deadline - time.monotonic()))):
            if self.process.poll() is not None or time.monotonic() >= deadline:
                self.stop(check=False)
                raise RuntimeError(f"gateway startup failed\n{self.stdout}\n{self.stderr}")
        return self

    def stop(self, *, check: bool = True) -> None:
        if self.process is None:
            return
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=self.timeout)
            except subprocess.TimeoutExpired as error:
                self.process.kill()
                self.process.wait()
                raise RuntimeError("gateway did not shut down within deadline") from error
        for thread in self._threads:
            thread.join(timeout=self.timeout)
        for label in ("stdout", "stderr"):
            stream = getattr(self.process, label)
            if stream is not None:
                stream.close()
        if check and self.process.returncode != 0:
            raise RuntimeError(f"gateway exited {self.process.returncode}\n{self.stdout}\n{self.stderr}")

    def __exit__(self, exc_type: object, *_: object) -> None:
        self.stop(check=exc_type is None)
