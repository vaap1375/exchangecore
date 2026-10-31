# Performance evidence

Measured locally on **2026-10-29**. These are synthetic, single-threaded engine
measurements, not production venue or network latency claims. All workloads and
both hash-reservation configurations passed deterministic replay and state checks.

## Environment and reproducibility

- Apple M1 Max, 10 cores (8 performance / 2 efficiency), 32 GB RAM.
- macOS 26.6.2, Darwin 25.6.0 arm64.
- Apple Clang 21.0.0 (`clang-2100.3.34.2`), libc++, C++20.
- CMake 4.4.4, Ninja 1.13.2; Release `-O3 -DNDEBUG`, no `-march=native`, LTO,
  sanitizer or automatic invariant checks.
- Seed 60448; **1,000,000 events per workload**, seven throughput trials, two
  warmup replays, 10,000 separate stratified latency samples.
- Sequential measurements. CPU affinity, core type, power state, allocator cache
  state and OS scheduling were not controlled. Other machine activity can affect
  results. Keep the variation and reverse-order comparisons when citing numbers.

Build and reproduce both configurations:

```sh
cmake --preset release
cmake --build --preset release
scripts/benchmark.sh 1000000
python3 scripts/compare_benchmarks.py benchmark-results/baseline.csv benchmark-results/reserved.csv
```

The script runs baseline then reserved. The recorded repetition reversed that
order, using the same executable/options:

```sh
build/release/exchangecore_bench --events 1000000 --trials 7 --warmup 2 --samples 10000 --seed 60448 --reserve 1000000 --csv > benchmark-results/reserved-reverse.csv
build/release/exchangecore_bench --events 1000000 --trials 7 --warmup 2 --samples 10000 --seed 60448 --reserve 0 --csv > benchmark-results/baseline-reverse.csv
```

Raw CSVs are preserved in [measurements](measurements/). Files with `-1m` are the
million-event runs; files without that suffix are the initial 100k experiment.
`-reverse` denotes the second process order. Source/configuration is unchanged
between each pair. No benchmark results were discarded or edited.

## Million-event baseline

Throughput is **input events/second**, including new limits, markets and cancels
as specified by each workload. M means million. CV is population standard
deviation divided by mean across the seven trial rates.

| Workload | Min M/s | Median M/s | Max M/s | CV |
|---|---:|---:|---:|---:|
| Resting | 10.672 | 11.179 | 11.312 | 2.44% |
| Crossing | 8.642 | 8.913 | 8.975 | 1.22% |
| Cancellation | 15.851 | 16.202 | 16.382 | 1.00% |
| Deep book | 7.749 | 7.833 | 8.107 | 1.39% |
| Near best prices (`touch`) | 11.910 | 12.255 | 12.348 | 1.12% |
| Mixed | 14.197 | 14.520 | 14.804 | 1.26% |

The reverse-order baseline medians were 10.540, 8.775, 16.050, 7.691, 11.971 and
14.356 M/s respectively. Mixed throughput therefore repeated at approximately
**14.4–14.5 million events/second** for this million-event setup. The deep workload
creates one million active orders at one million distinct levels; the mixed
workload performs 300,000 trades and 200,000 successful cancellations and ends empty.

Separate instrumented baseline latency samples (nanoseconds):

| Workload | Average | p50 | p95 | p99 |
|---|---:|---:|---:|---:|
| Resting | 183.6 | 83 | 125 | 167 |
| Crossing | 162.0 | 84 | 250 | 292 |
| Cancellation | 74.9 | 83 | 125 | 125 |
| Deep book | 213.2 | 125 | 167 | 209 |
| Near best prices | 90.5 | 83 | 125 | 125 |
| Mixed | 79.1 | 83 | 125 | 125 |

These samples include clock instrumentation and moving the returned result,
excluding its later destruction. Percentiles have visible timer quantization
(many multiples near 42 ns), despite the clock's nominal 1 ns period. Do not infer
single-nanosecond precision. Rare large delays can raise the mean above p99.
This is one separate latency replay, not a confidence interval or a rigorous
tail-latency service guarantee. Throughput and latency are not reciprocals.

## Controlled optimization: reserve the ID indexes

`EngineConfig.reserve_orders` calls `reserve` on the active-order hash map and
lifetime-ID set during construction. It leaves matching and ownership unchanged.
`--reserve 0` preserves the baseline; `--reserve 1000000` tests preallocation for
the million-event experiment. Reservation is capped by `max_orders`.

| Workload | Baseline median M/s | Reserved median M/s | First change | Reverse-order change |
|---|---:|---:|---:|---:|
| Resting | 11.179 | 12.152 | +8.71% | +11.43% |
| Crossing | 8.913 | 9.247 | +3.76% | +3.96% |
| Cancellation | 16.202 | 16.169 | −0.20% | +1.97% |
| Deep book | 7.833 | 8.317 | +6.18% | +11.60% |
| Near best prices | 12.255 | 12.755 | +4.08% | −1.63% |
| Mixed | 14.520 | 15.070 | +3.79% | +2.00% |

The initial 100k experiment showed smaller apparent gains that mostly vanished
in reverse order. Larger books produced repeated gains for deep/resting insertion
and crossing. This supports **configurable** reservation when expected volume is
known. It does not support enabling maximum reservation for every caller. Default
remains zero. Reverse million-event CV reached 14.26% (reserved resting), 18.59%
(reserved touch), and 10.82% (reserved mixed), so the table is evidence for a
workload-specific experiment, not a universal or statistically isolated speedup.

Allocation replay provides a more direct explanation and a memory tradeoff.
Construction adds two bucket allocations requesting 16,000,048 bytes. Gross
successful requested bytes, **including construction**:

| Workload | Baseline bytes | Reserved bytes | Difference |
|---|---:|---:|---:|
| Resting | 180,686,352 | 144,009,264 | −36,677,088 |
| Crossing | 210,338,736 | 200,000,160 | −10,338,576 |
| Cancellation | 113,168,688 | 116,000,048 | +2,831,360 |
| Deep book | 252,677,136 | 216,000,048 | −36,677,088 |
| Near best prices | 145,005,288 | 134,666,752 | −10,338,536 |
| Mixed | 112,368,728 | 115,200,048 | +2,831,320 |

For deep books, processing allocation calls fall from 4,000,040 to 4,000,000;
reservation removes rehash allocations, not the per-order node allocations.
Resting processing calls fall from 3,000,168 to 3,000,128. Over-reserving the
active index on short-lived workloads can increase total requests. Construction
is excluded from throughput, so its real application cost must also be considered.

The maximum reported process RSS in the first million-event process was 380,704
KiB baseline and 335,696 KiB reserved. RSS is cumulative process high-water usage
across workloads, event arrays, validation, snapshots and allocator retention;
it is **not** a per-book memory measurement. Gross allocation requests are not
live/retained bytes or a memory-leak result.

## Profiling

`python3 scripts/profile_macos.py` sampled a Release deep-book child process with
200,000 events and 100 trials for three seconds at 1 ms intervals. The first
sandboxed attempt could not inspect the process. A scoped access escalation
succeeded and captured **2,464 main-thread stacks**. The ignored local raw output
is `build/profile-sample.txt`; a portable excerpt is preserved in
[profile-summary.txt](measurements/profile-summary.txt).

Top-of-stack observations included tree rebalance (248), `OrderBook::add` (233),
tiny allocation (229), `_malloc_zone_malloc` (118), plus significant deallocation,
snapshot and invariant-check work. This identifies tree/node allocation and hash
growth as plausible areas for controlled experiments. It does not isolate pure
matching CPU percentages: the sampled whole process also performs untimed
validation, snapshots and teardown. Automatic Release checks remain disabled;
the harness deliberately checks invariants outside timed intervals.

A custom pool was not added: it would change ownership and exception behavior,
and these observations alone do not establish a net benefit. Standard RAII
containers, measurable reservation and honest limits provide a useful baseline
for future memory-layout experiments. On Linux, use `perf record -g --` with the
Release executable where supported; Linux profiling has not been run locally.

## Measurement boundaries

Throughput excludes workload generation, engine construction/destruction, parsing,
printing, full trade hashing and invariant checks. It includes processing,
returned-result destruction and a small observable scalar digest. Latency and
allocation instrumentation run in separate replays. The standalone binary's
disabled allocation-counter branch still has a cost; both configurations use
that same binary. Full per-trial raw rates are summarized as min/median/max/CV,
not retained individually in CSV. See [methodology](../benchmarks/README.md).

In this original core experiment, no CPU pinning, Linux performance comparison,
hardware-counter analysis, production traffic
trace or end-to-end exchange test was performed. Reproduce on the target machine
before transferring performance claims.

## Optional TCP gateway upgrade

The original measurements above remain preserved. The gateway uses separate
targets and leaves core matching and benchmark source unchanged. Fresh core
before/after measurements, TCP scaling at 1/4/8/16 clients, a separate overload
experiment, RTT percentiles and memory/queue observations are documented in
[NETWORK_PERFORMANCE.md](NETWORK_PERFORMANCE.md). End-to-end Python/TCP rates
must not be substituted for the in-process engine numbers above. Linux
Debug/Release and Clang ASan/UBSan/LeakSanitizer compatibility verification are
now recorded in `IMPLEMENTATION_STATUS.md`; Linux performance is not measured.

The [2026-10-31 audit](VERIFICATION_2026-10-31.md) repeats both suites with the
updated compiler. Baseline and current core benchmark executables are identical
in that build; both process orders and the observed timing spread are preserved
without attributing the differences to a code change.
