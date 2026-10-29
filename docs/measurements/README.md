# Local measurements, 2026-10-29

These CSVs contain measurements from the local Apple Clang 21.0.0 Release build
on Darwin 25.6.0 arm64. The harness reports 10 hardware threads; CPU affinity,
frequency locking and background-load isolation were not applied. They are
synthetic, single-threaded engine microbenchmarks, not exchange/network latency
measurements. The source repository was uncommitted when these were produced.

The detailed tables below cover the **initial exploratory 100000-event runs**.
They are distinct from the later 1000000-event scaling experiment, whose raw
files are `baseline-1m.csv` and `reserved-1m.csv` (with reverse-order repeats when
present). Use [the performance report](../PERFORMANCE.md) for the final scaling
results and engineering conclusions.

[profile-summary.txt](profile-summary.txt) records a separate macOS `sample`
capture with 2464 main-thread stack observations and the separately recorded
Apple M1 Max CPU identity. It samples the complete benchmark process, including
preparation, invariant checks, snapshots and destruction outside throughput
timing. Its tree-balancing and allocation/free observations help identify areas
to investigate, but cannot be quoted as isolated matching CPU percentages.

## Raw files and execution order

The first comparison ran [baseline.csv](baseline.csv), then
[reserved.csv](reserved.csv). The second comparison reversed the configuration
order: [reserved-reverse.csv](reserved-reverse.csv), then
[baseline-reverse.csv](baseline-reverse.csv). Each CSV came from a fresh
standalone benchmark process, running all six workloads in the same order.

Common options were `--events 100000 --trials 7 --warmup 2 --samples 10000
--seed 60448 --csv`. Baseline used `--reserve 0`; reserved used
`--reserve 100000`. For example:

```sh
build/release/exchangecore_bench --events 100000 --trials 7 --warmup 2 --samples 10000 --seed 60448 --reserve 0 --csv
build/release/exchangecore_bench --events 100000 --trials 7 --warmup 2 --samples 10000 --seed 60448 --reserve 100000 --csv
```

All four files have identical result hashes, final-state hashes and activity
counters for each corresponding workload. Timing naturally differs. Raw CSVs
store summary statistics, not individual trial durations or latency samples.

## Initial 100000-event baseline throughput

This initial baseline table uses `baseline.csv`, chosen before analyzing the repeat.
Units are **million input events per second**; an input event may produce zero,
one or several executions. CV is the population coefficient of variation across
seven throughput trials.

| Workload | Minimum | Median | Maximum | CV (%) |
| --- | ---: | ---: | ---: | ---: |
| resting | 10.505 | 11.125 | 11.542 | 2.811 |
| crossing | 9.094 | 9.125 | 9.180 | 0.312 |
| cancellation | 15.191 | 16.478 | 16.651 | 3.106 |
| deep | 3.788 | 8.525 | 8.826 | 21.319 |
| touch | 12.371 | 12.498 | 12.611 | 0.648 |
| mixed | 14.781 | 14.807 | 14.938 | 0.473 |

The deep workload had a substantial low outlier (3.788 million events/s,
CV 21.319%); its repeat baseline median was 8.887 million events/s with CV 1.889%.
The primary baseline medians span 8.525–16.478 million events/s. The mixed
workload measured 14.807 million events/s here and 15.043 million events/s in the
repeat baseline. These are observations on this host, not guarantees.

## Initial 100000-event baseline sampled latency

Units are instrumented nanoseconds. Sampling uses a separate replay and 10000
seeded, stratified event positions. Reported percentiles use nearest rank.

| Workload | Average | p50 | p95 | p99 |
| --- | ---: | ---: | ---: | ---: |
| resting | 95.505 | 84 | 125 | 125 |
| crossing | 122.932 | 84 | 250 | 292 |
| cancellation | 75.123 | 83 | 125 | 125 |
| deep | 127.115 | 125 | 167 | 167 |
| touch | 97.830 | 83 | 125 | 125 |
| mixed | 78.632 | 83 | 125 | 125 |

The clock's nominal representation is one nanosecond, but the observed
percentiles cluster around approximately 42 ns increments (83/84, 125, 167,
250 and 292 ns). Representation does not establish effective timing resolution.
Per-call clock reads, sampling branches and moving returned results perturb
very short operations. Avoid claiming exact tens-of-nanoseconds uninstrumented
latency from these quantized values. Throughput and sampled latency measure
different timing boundaries; see [the methodology](../../benchmarks/README.md).

## Initial 100000-event reserve experiment

Median throughput change is `100 * (reserved / baseline - 1)` within each
comparison. Positive values favor reservation.

| Workload | Baseline then reserved | Reserved then baseline |
| --- | ---: | ---: |
| resting | 8.71% | 0.36% |
| crossing | 2.10% | -0.83% |
| cancellation | 2.00% | -3.57% |
| deep | 4.68% | -1.24% |
| touch | 2.52% | -0.21% |
| mixed | 2.44% | 0.56% |

The apparent first-run gains were not stable after reversing process order.
The reverse-order cancellation reserved run also had a large low outlier
(CV 16.518%). These two comparisons do not establish a general throughput
improvement or statistical significance. Default reservation remains zero;
explicit reservation remains available as a reproducible experimental option.

Allocation counters were identical between the two runs of each configuration.
Baseline engine construction made zero counted allocations. Reserved construction
made **2 allocations requesting 1600048 bytes** before event processing.
The following totals include both construction and processing, so moving work
outside the throughput interval cannot be mistaken for eliminating all of it.

| Workload | Baseline calls | Reserved calls | Baseline requested bytes | Reserved requested bytes | Byte change |
| --- | ---: | ---: | ---: | ---: | ---: |
| resting | 300160 | 300130 | 16100112 | 14409264 | -1690848 |
| crossing | 366686 | 366670 | 20045616 | 20000160 | -45456 |
| cancellation | 200016 | 200002 | 10822448 | 11600048 | +777600 |
| deep | 400032 | 400002 | 23290896 | 21600048 | -1690848 |
| touch | 266685 | 266670 | 13512168 | 13466752 | -45416 |
| mixed | 230018 | 230002 | 11565504 | 11520048 | -45456 |

During processing alone, reservation eliminated 32 calls / 3290896 requested bytes
in resting and deep; 18 / 1645504 in crossing and mixed; 16 / 822448 in cancellation;
and 17 / 1645464 in touch. After including construction, resting and deep still
requested 1690848 fewer bytes, whereas cancellation requested 777600 more bytes.
The cancellation workload accepts only 50000 new orders and has at most two
active orders, so reserving both hash indexes for 100000 orders overprovisions it.

These are **gross successful C++ allocation requests**, not peak/live memory.
Counters do not see every libc or OS allocation. The disabled instrumentation
branch still exists in the standalone executable during timing; configurations
were compared with that same executable. RSS values are the whole-process
lifetime high-water mark and include workload arrays, validation snapshots and
earlier workloads. They do not identify per-workload retained memory or establish
a standalone RSS improvement from reservation.

## Defensible phrasing for the initial 100000-event run

A resume may state: “Benchmarked six deterministic 100000-event workloads at
8.5–16.5 million input events/s median over seven Release trials on local arm64
hardware.” Retain the synthetic/local qualifier and link these measurements.
For a more stable representative workload: mixed measured 14.8–15.0 million input
events/s median in the two baseline processes.

Do not claim a proven percentage throughput optimization, real-world exchange
latency, statistical confidence, or a particular hardware CPU model from these
CSVs alone. Reservation measurably reduces hash-growth allocation requests in some
workloads, but throughput gains were inconsistent and cancellation gross
requested bytes increased.

