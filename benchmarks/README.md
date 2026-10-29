# Benchmark methodology

Use the Release preset and the same executable, options, hardware and background
load when comparing configurations. No measurements are embedded in this file;
verified measurements belong in `docs/PERFORMANCE.md`.

```sh
cmake --preset release
cmake --build --preset release
build/release/exchangecore_bench --events 100000 --trials 7 --warmup 2 --samples 10000
build/release/exchangecore_bench --events 100000 --trials 7 --warmup 2 --samples 10000 --csv
```

`exchangecore benchmark` runs the same engine workload suite. Allocation
instrumentation is available in `exchangecore_bench`, whose executable alone
replaces global C++ allocation functions. Both commands support `--help`.

## Workloads

All event arrays are constructed before measurement. `mt19937_64` outputs with
explicit integer modulo operations make the arrays reproducible across standard
library implementations. Seeds vary quantities and, where appropriate, prices.
IDs increase deterministically and are never reused for new orders.

| Name | Activity |
| --- | --- |
| `resting` | Noncrossing buy and sell limits spread across 128 possible price levels. All orders remain active. |
| `crossing` | Six-event blocks: three ask levels, one aggressive limit sweeping all three, one bid and a sell market fill. |
| `cancellation` | Two noncrossing limits followed by cancellation of both. Half the events cancel; no matching occurs. |
| `deep` | One distinct, noncrossing price level per event, alternating sides. Exercises tree insertion and large resident books. |
| `touch` | Six-event blocks concentrated at bid 23999 and ask 24001: limits, aggressive fills, market fills and cancellation. |
| `mixed` | Ten-event blocks combining partial fills, aggressive limits, markets, cancellation and an unmatched market order. |

Every complete block in the four block-based workloads empties the book. The
final block may be truncated; the resulting partial state is valid and hashed.
Workloads are synthetic, deliberately structured microbenchmarks. They do not
represent real exchange traffic or measure network, gateway or durable-log costs.

## Correctness gates

Before measurement, every workload is replayed with full result and trade
hashing, checked for expected activity, and checked with engine invariants. For
example, `deep` must create exactly one price level per event and `cancellation`
must cancel the expected number of orders. A second independent replay verifies
the complete result hash and final-state hash. Configured warmup replays follow
the same path. Warmup zero still performs both correctness replays.

Every throughput, latency and allocation replay checks an inexpensive scalar
result digest and final-state hash against that validation replay. Invariants
and state hashing execute outside timed regions. Full trade hashes appear as
`replay_checksum`; `state_checksum` includes final orders, quantities, sides,
prices and arrival sequences. These hashes detect regression and replay drift;
independent reference-model tests establish matching correctness.

## Timing

Throughput uses one `steady_clock` interval around an entire replay, with a fresh
engine for every trial. Construction and destruction of the engine, workload
generation, input parsing, validation, formatting and terminal output are excluded.
The timed loop includes `process`, destruction of each returned result, and a
small integer reduction of its filled, remaining, canceled and trade-count fields.
This reduction keeps results observable without hashing individual trades in the
timed region. Reported operations are **input events**, not executions or fills.

Results report minimum, median and maximum operations per second and population
coefficient of variation across trials. With an even trial count, the median is
the arithmetic mean of the central two sorted rates. A single trial has zero
reported variation and does not establish stability.

Latency uses a **separate replay**, recording only selected calls. One seeded
sample is chosen in each equal-sized interval of the input stream. Stratified
sampling avoids selecting only the same operation in a periodic workload. Each
sample has two clock reads around `process` and moving its result; destruction
of that result follows the second clock read. Nonsampled operations still run
to preserve the same book state. The average is the arithmetic mean of sampled
nanoseconds. p50, p95 and p99 use the nearest-rank definition.

Per-operation clocks, sampling branches, cache effects and clock granularity
distort very short operations. Latency samples are diagnostic measurements;
do not equate them with uninstrumented engine latency, reciprocal throughput,
or real exchange end-to-end latency. `--samples` clamps to the event count.

## Allocations and memory

The standalone executable provides all ordinary, array, aligned, nothrow and
applicable sized allocation/deallocation replacements. Allocation functions use
`malloc`/`posix_memalign`, retain `new_handler` behavior and use thread-local
counters. Only an explicitly enabled **separate, untimed replay** increments
counters. There are two scopes: engine construction, and processing the event
array. Each reports successful allocation calls and gross requested bytes.
Workload generation, validation, final snapshots and engine destruction are
outside these scopes. Results include returned trade-vector allocations.

Gross requested bytes are neither retained memory nor peak live bytes. The
instrumentation sees C++ allocation calls in this executable, not all libc or OS
allocations. During timed runs its disabled counter branch still executes on
allocation; compare baseline and reserve using the same executable. The ordinary
CLI has no allocator replacement and explicitly reports unavailable instrumentation
(`allocations_available=0` in CSV), whose accompanying zeros are placeholders.

`process_peak_rss_kib` uses `getrusage(RUSAGE_SELF).ru_maxrss`: bytes are converted
to KiB on macOS; Linux already reports KiB. It is the **whole process lifetime
high-water mark**, including event arrays and earlier workloads, so it is not a
per-workload allocation estimate. Zero indicates the platform measurement was
unavailable. Use separate processes to compare memory configurations.

## Controlled reserve comparison

Default `--reserve 0` preserves normal dynamically growing hash indexes. An
explicit reservation moves hash-bucket allocation to construction, which is
excluded from throughput but reported in the separate initialization allocation
scope. It also reserves the active-order index even when a matching-heavy book
has few active orders; this can waste memory. The lifetime accepted-ID index
grows with all accepted new orders. `max_orders` is configured to cover all
requested benchmark events and reservations in either case.

```sh
build/release/exchangecore_bench --events 100000 --trials 7 --warmup 2 --samples 10000 --reserve 0 --csv
build/release/exchangecore_bench --events 100000 --trials 7 --warmup 2 --samples 10000 --reserve 100000 --csv
```

Alternate comparison order and use several fresh-process runs to reduce thermal,
allocator and background-load bias. Keep result and state checksums identical.
Retain optimizations only when correctness still passes and measurements support
the stated workload-specific benefit. Reservation is a configuration choice,
not a universal speedup; construction time and memory costs must remain visible.

## CSV and reproducibility

CSV contains one header followed by one row per selected workload, with numeric
configuration, trial summaries, sampled latency, activity counters, hashes,
allocation scopes, RSS and platform/compiler/build metadata. String fields are
quoted and embedded quotes escaped. The steady-clock period and hardware thread
count are recorded. CPU model, power policy, optimization flags and background
load should also be recorded by the person running a published comparison.
Debug output identifies itself; use Release builds for performance claims.

Event counts are bounded to 64..5000000, trials to 1..100, warmups to 0..100, and
reservations to 0..5000000. Runs are single-threaded and deterministic in matching
behavior; wall-clock results naturally vary. No CPU affinity, frequency locking
or statistical confidence interval is imposed by the harness.
