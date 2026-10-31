# Testing and verification

The suite uses a dependency-free registry harness so configure/build never needs
a network connection. Checks remain active in Release; it does not rely on the
standard `assert` macro.

```sh
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset release && cmake --build --preset release && ctest --preset release
cmake --preset sanitize && cmake --build --preset sanitize && ctest --preset sanitize
```

Run `build/debug/exchangecore_tests` for named case output. It covers empty and
noncrossing books, both sides, full/partial/multiple fills, maker prices,
multi-level sweeps, FIFO, residual priority, cancellations and iterator survival,
retired IDs, invalid enums, quantity/price/ID boundaries, capacity, detached
snapshots, aggregate depth and invariant checks. The parser tests cover headers,
field counts, signs, overflow, comments, whitespace, CRLF, no final newline,
diagnostic limits, line limits and deterministic replay. Price formatting tests
include signed integer extremes without using floating point.

## Independent model

`tests/reference_model.hpp` uses a flat vector, a linear ID search and a linear
best eligible maker selection. It does not share the production map/list indexes,
matching code or validation function. A vector of retired IDs models ID lifetime.

Eight regimes (low/high liquidity, heavy matching/cancellation, deep/shallow books,
invalid input and mixed) each use seeds `1`, `42`, `0xc0ffee`, `0xdeadbeef1234` and
1,500 events per seed: 48,000 events total. After each event the test compares full
`Result` equality (including ordered trades and rejection reason), best prices,
order snapshots with sequence/quantity, depth, aggregate quantities, active IDs
and accepted counts, then checks production invariants.

Failures record metadata and the event prefix in `test-failures/` relative to the
test working directory. Syntactically representable prefixes can be replayed with
the CLI. API-only invalid enum events are reproduced using the fixed seed and
regime in the test harness. No failing
seed should be discarded merely to obtain a pass.

## Allocation failure

`exchangecore_exception_tests` isolates replacement global allocation functions in
its own process. It injects failure at successive allocation points for reserved
construction, first-level insertion, appending FIFO, partial fill, both-sided
multi-level sweeps with remainder and a full market sweep. A failed operation
must preserve order snapshots, best prices, accepted count, active count and
quantities. Retrying the same ID must match the unfaulted result and sequence.

ASan/UBSan apply to library, CLI and tests. Sanitizers are not suppressed. On Linux
CI, leak detection is enabled with `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`.
Apple's ASan configuration is used locally; a successful macOS run is not a
Linux LeakSanitizer run.

## TCP gateway tests

The optional gateway adds four CTest groups: `network_protocol` (33 named cases),
`bounded_queue` (12 cases including MPSC/MPMC stress), `network_integration`, and
`network_demo`. The original eight groups remain intact. Protocol parsing is
tested without sockets; queue tests use latches, close notifications and exact
delivery accounting rather than sleep-based coordination.

The independent Python integration client exercises live ephemeral loopback
servers: cross-client partial fills and cancellation, request correlation,
fragmented/coalesced messages, CRLF, the 4096-byte frame boundary, malformed and
oversized input, duplicate requests/order IDs, half-close and incomplete FIN,
rapid reset/reconnect and descriptor reuse, large streamed trade results through
512-byte output buffers and seven-byte sends, explicit queue-capacity-one
overload, slow-reader isolation, concurrent workloads, connection caps, graceful
draining, startup/bind errors and replay of recorded processing order.

The recorded-order test runs four concurrent clients, sorts their 192 requests
by returned processing sequence, then submits that exact order to a fresh server
and compares complete records, including query observations. It tests
determinism of the chosen sequence, not repeatability of concurrent arrival.

```sh
scripts/verify_network.sh
# Or target the new functionality in one preset:
ctest --preset debug -R 'network_|bounded_queue'
ctest --preset release -R network_integration --repeat until-fail:10
cmake --preset thread-sanitize
cmake --build --preset thread-sanitize
ctest --preset thread-sanitize

# Existing Docker runtime required; isolated Linux loopback, no published ports:
scripts/verify_linux.sh
```

The server announces its ephemeral port through a flushed readiness line. Tests
use barriers, events and I/O deadlines, avoiding fixed sleeps as readiness tests.
SIGTERM shutdown checks `accepted == processed`. Seven-byte send chunks force
fragmented output, while small kernel buffers and a slow reader exercise
nonblocking pressure. Counters verify queue, pending, input and output bounds.
These tests cannot prove delivery to a client that disconnects or reads no reply.

In the managed macOS environment, binding loopback required scoped sandbox
access; ordinary unrestricted local execution needs no privileged network port.
The new code's Clang static analysis emitted no findings. A real macOS failure
was reproduced during rapid reset: `SO_NOSIGPIPE` on a newly reset accepted
socket returned EINVAL. Per-connection setup errors now close only that socket;
the reset/reconnect regression remains in the suite.

Sanitizer configurations use `-O1`, debug symbols, frame pointers and all enabled
checks. GCC 13/Linux sanitizer runs at both `-O0` and `-O1` exceeded the reference
and CLI-smoke deadlines without a sanitizer finding. The exact cause remains
unproven. Linux validation therefore uses Clang 18, which passes unchanged
coverage with leak detection enabled; no checks or findings are suppressed.
ThreadSanitizer is a separate preset and cannot be combined with ASan.

The [2026-10-31 audit](VERIFICATION_2026-10-31.md) records fresh 12/12 passes in
all four native configurations and all three Linux configurations, plus ten
complete 13-scenario TCP repetitions. No sanitizer findings occurred in these
successful runs. The earlier implementation evidence remains below.

The gateway-off configuration also builds and runs the original eight groups:

```sh
cmake -S . -B build/core-only -G Ninja -DCMAKE_BUILD_TYPE=Release -DEXCHANGECORE_BUILD_GATEWAY=OFF
cmake --build build/core-only
ctest --test-dir build/core-only --output-on-failure
```

## CLI and benchmark contracts

Python black-box tests exercise actual process exit codes, path/line errors,
empty and malformed files, semantic rejection, quiet replay, identical replay
output, demo output and benchmark option bounds. Python is optional for library
consumers; CMake registers these tests when an interpreter is found. Benchmark
generators and checksums also validate activity so a fast no-op workload cannot
silently masquerade as matching performance.

## Evidence

Original core verification on 2026-10-29: **8/8 CTest groups passed** in Debug,
Release and combined ASan/UBSan. Within those groups: 67 named core/parser/price/
reference cases, 48,000 differential events, seven allocation scenarios with
31 injected failure points, four generator groups spanning 594 combinations, and
22 CLI process checks. `scripts/verify.sh` also validated all four example files
and ran all six standalone benchmark workloads as smoke checks.

Clang static analysis of the four library sources produced no diagnostics:

```sh
clang++ --analyze -std=c++20 -Iinclude -Xanalyzer -analyzer-output=text src/order_book.cpp src/matching_engine.cpp src/csv.cpp src/price.cpp
```

An installed `find_package(ExchangeCore CONFIG REQUIRED)` consumer compiled and
ran using matching, parsing and price formatting APIs. Final executed counts,
configurations and limitations are recorded in `IMPLEMENTATION_STATUS.md`;
exact measured benchmark runs are in `PERFORMANCE.md`.
Hosted CI is configured but has not run because no remote/push was authorized.

Sequence exhaustion is guarded in production but cannot feasibly be reached by
a black-box test iterating 2^64 accepted orders. Validation guards and ordinary
sequence semantics are tested. Randomized tests give reproducible evidence, not
a proof over every possible event stream.
