# ExchangeCore

A deterministic C++20 limit-order matching engine with price-time priority,
indexed cancellation, an independent reference model, allocation-failure tests,
and reproducible performance experiments. A reusable systems library and CLI,
plus an optional POSIX TCP gateway with bounded queues and a dedicated matching
thread. No external services or real-market connectivity.

The project focuses on a small, difficult problem: maintain exact quantities and
stable FIFO priority while orders cross multiple levels, fills remove nodes,
cancellations invalidate iterators, and invalid input or allocation failures
must preserve state.

## Build and run

Requires a C++20 compiler, CMake 3.21+ for presets, and Ninja. macOS and Linux
are supported targets. No third-party C++ dependencies or downloads at configure
time. Python 3 is optional for black-box CLI tests; the network demo and benchmark
tools require Python 3.10+.

```sh
# Optional local tooling, if CMake/Ninja are missing:
python3 -m venv .venv
.venv/bin/python -m pip install cmake ninja
export PATH="$PWD/.venv/bin:$PATH"

cmake --preset release
cmake --build --preset release
build/release/exchangecore demo
build/release/exchangecore replay examples/orders.csv
build/release/exchangecore validate examples/mixed.csv
build/release/exchangecore --help
```

Without Ninja/presets: `cmake -S . -B build/local -DCMAKE_BUILD_TYPE=Release`,
then `cmake --build build/local` and `ctest --test-dir build/local --output-on-failure`.

## Optional TCP order entry

```sh
# Automatic two-client demonstration; starts an ephemeral loopback server:
python3 scripts/network_demo.py

# Or start a server, then use a second terminal:
build/release/exchangecore_gateway --host 127.0.0.1 --port 9000
python3 scripts/network_client.py --port 9000 --command 'LIMIT 1 SELL 10 24001' --command BEST
python3 scripts/network_client.py --port 9000 --command 'MARKET 2 BUY 4' --command BEST
# Ctrl-C gracefully stops admission, drains matching, then flushes replies.

# Separate end-to-end loopback benchmarks:
python3 benchmarks/network_benchmark.py --clients 1,4,8,16 --workloads ping,matching,cancellation --trials 3
```

`EC1` is a bounded line protocol with request IDs, explicit processing sequences,
LIMIT/MARKET/CANCEL, BEST and PING. One nonblocking `poll()` thread owns every
socket; a dedicated writer owns the original matching engine. Bounded request
and completion queues, per-client pending limits and incremental response
streaming constrain memory. Full queues reject with `OVERLOADED`; slow clients
have reads paused. A successful queue admission receives no acknowledgment until
processing completes. Accepted work continues after disconnect, with generation
tokens preventing replies from reaching a reused connection.

This simulated gateway defaults to IPv4 loopback. It has no TLS, authentication,
per-client order ownership or durable journal and is not a secure public endpoint.
Concurrent network arrival order can differ between runs; a recorded processing
sequence replays deterministically. See [wire protocol](docs/NETWORK_PROTOCOL.md)
and [ownership/backpressure](docs/CONCURRENCY.md).

Disable all gateway targets with `-DEXCHANGECORE_BUILD_GATEWAY=OFF`. The installed
core library and original CLI have no networking-thread dependency or added locks
in the matching path.

Local verification passed all 12 CTest groups on macOS Debug/Release/ASan+UBSan/
TSan and Linux GCC Debug/Release plus Clang ASan+UBSan+LeakSanitizer. The 13-case
TCP suite also passed ten consecutive repetitions. Hosted CI remains unrun.
Linux verification uses Clang for sanitizers because GCC's instrumented reference
tests exceeded their deadlines; that limitation is documented.

On Apple M1 Max, the 2026-10-31 four-client matching audit measured **39,970 completed
TCP requests/s**, with **781/1,138/1,310 µs p50/p95/p99 RTT**, at pipeline depth
eight per client. This includes Python client, socket, parsing and scheduling
costs; it is separate from core engine throughput. See
[latest verification and measurements](docs/VERIFICATION_2026-10-31.md) and
[TCP methodology, overload and memory evidence](docs/NETWORK_PERFORMANCE.md).

## Matching behavior

- Buys take the lowest ask; sells take the highest bid; FIFO at equal prices.
- Trades execute at the resting order's price. Limits can sweep multiple levels.
- Partial limit remainders rest; unfilled market remainders expire.
- Accepted IDs remain consumed after fills/cancels; rejected IDs can be retried.
- Positive integer price ticks and quantities; exact overflow checks.
- Explicit acceptance/rejection results, ordered trades, detached depth and
  order snapshots. No console output or wall-clock data in core matching.

The deterministic demo shows a sell of ten, a buy filling four, a second buy
consuming the remaining six before the next same-price maker, further executions
at another price, and a cancellation. It prints updated best prices and depth
after each event.

CSV is deliberately strict and dependency free:

```csv
type,id,side,quantity,price
LIMIT,1,SELL,10,24001
MARKET,2,BUY,4,
CANCEL,1,,,
```

Prices are integer ticks: `24001` represents `6000.25` at 0.25 points/tick.
CSV errors include file and line. `validate` checks semantics too, including
duplicate IDs and unknown cancellations. See [the complete contract](docs/MATCHING_RULES.md).

## Library and architecture

```cpp
#include <exchangecore/matching_engine.hpp>

exchangecore::MatchingEngine engine;
auto resting = engine.submit_limit(1, exchangecore::Side::Sell, 10, 24001);
auto fill = engine.submit_market(2, exchangecore::Side::Buy, 4);
// fill.trades[0] = {maker_id=1, taker_id=2, price=24001, quantity=4}
auto remaining = engine.find(1);  // quantity = 6
auto asks = engine.depth(exchangecore::Side::Sell, 5);
```

Link the in-tree target `ExchangeCore::exchangecore`. To consume an installation:

```sh
cmake --install build/release --prefix "$PWD/build/install"
```

Then `find_package(ExchangeCore CONFIG REQUIRED)` and link
`ExchangeCore::exchangecore`, setting `CMAKE_PREFIX_PATH` to the install prefix.

Each side uses `std::map<Price, PriceLevel>` with `std::list<RestingOrder>` per
level. A hash map stores ID-to-level/list iterators; a separate set tracks lifetime
IDs. Containers own memory through RAII; queries expose copies. Matching prepares
results and stages allocations before committing executions, allowing rollback
on `std::bad_alloc`.

| Operation | Complexity |
|---|---|
| Best bid / ask | O(1) |
| Rest noncrossing limit | O(log L), expected amortized hash insertion |
| Match K makers | O(K + D) for D emptied levels, plus remainder insertion |
| Cancel active order | Expected O(1), amortized iterator-based level erase |
| Lookup order | Expected O(1) |
| Depth H levels | O(min(H,L)) |
| Snapshot N orders | O(N) |

Hash operations have linear worst cases. Lifetime ID memory grows with accepted
orders, bounded by configuration (one million by default). See
[data structures](docs/DATA_STRUCTURES.md) and [architecture](docs/ARCHITECTURE.md)
for ownership, iterator validity and full complexity assumptions.

## Correctness and memory safety

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

cmake --preset sanitize
cmake --build --preset sanitize
ctest --preset sanitize

# Debug + Release + ASan/UBSan, examples and benchmark smoke:
scripts/verify.sh

# Includes TCP integration, TSan, and ten repeated concurrent stress runs:
scripts/verify_network.sh
# Optional local Docker Linux compatibility and leak checks:
scripts/verify_linux.sh
```

The dependency-free suite covers matching rules, lifecycle, arithmetic boundaries,
iterator survival, snapshots and malformed CSV. Eight randomized regimes across
four seeds compare 48,000 events against a separate vector/linear-scan reference
model, checking full results and state after every event. Failing seeds and event
streams are saved for reproduction. Isolated fault injection checks allocation
rollback and same-ID retry. Debug and sanitizer presets enable automatic book
invariant checks; Release removes those checks from the hot path.

See [testing evidence and commands](docs/TESTING.md). GitHub Actions is configured
for Linux/macOS Debug/Release and Linux sanitizers; hosted CI remains unverified
until this local repository is published and the workflow runs.

## Performance experiments

```sh
build/release/exchangecore benchmark --events 100000 --trials 7
build/release/exchangecore_bench --events 100000 --trials 7 --samples 10000
scripts/benchmark.sh 100000
```

Six seeded workloads cover resting orders, crossing, cancellation, deep books,
activity near the best prices and mixed events. Throughput timing excludes
generation, engine construction, parsing, printing and full validation. Sampled
latency and allocation counting run separately. The standalone benchmark reports
gross allocation requests and process peak RSS; the library/CLI does not replace
global allocation functions. A small result digest prevents dead-code removal.

Measured results, hardware, statistical variation, the controlled hash-reservation
experiment and profiling evidence are recorded in [performance](docs/PERFORMANCE.md).
Synthetic engine throughput is not end-to-end exchange latency.

Local Apple M1 Max baseline, one million events per workload, median of seven
Release trials (2026-10-29):

| Workload | Million events/s |
|---|---:|
| Resting | 11.18 |
| Crossing | 8.91 |
| Cancellation | 16.20 |
| Deep book | 7.83 |
| Near best prices | 12.25 |
| Mixed | 14.52 |

The reverse-order mixed baseline repeated at 14.36M/s. Configurable hash
reservation improved million-event deep-book medians by 6.2% and 11.6% across
two run orders, with variation and upfront memory costs documented in the report.
Small-book improvements did not consistently reproduce; reservation remains opt-in.

## Scope and limitations

The engine intentionally has one serialized writer, one book, no venue-specific
order types, no modify/replace, no fees, no real market feed or durable event journal.
It is an educational systems project, not a production trading venue. Read-only
snapshots are copied and require caller serialization. CSV replay is bounded and
loaded before execution. Aggregate quantity validation is conservative at integer
limits; see the matching contract. Future work should follow evidence: broader
property testing, Linux profiling, workload-specific memory layout experiments,
and explicit priority rules before implementing replace.

[Implementation status](docs/IMPLEMENTATION_STATUS.md) and
[continuation instructions](docs/NEXT_STEPS.md) record the verified checkpoint.
[Resume notes](docs/RESUME_NOTES.md) distinguish measured accomplishments from
limitations.
