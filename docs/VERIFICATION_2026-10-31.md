# Verification audit — 2026-10-31

The existing networking implementation was inspected and preserved. No production
source changes were needed. HEAD remains the owner’s baseline commit `b9d0f24`;
networking changes remain uncommitted, with no remote or publication. Original
matching, API and benchmark sources compare unchanged against that commit.

## Current toolchain and tests

The installed Apple compiler changed from clang-2100.3.34.2 to
clang-2100.4.1.1. Clean native audit builds were created under
`build/audit-2026-10-31/`; the prior builds and measurements were preserved.
Hardware: Apple M1 Max, 10 cores (8 performance / 2 efficiency), 32 GB RAM.
Native OS: macOS 26.6.2 arm64. Python client: 3.14.7.
Native audit compiler: Apple Clang 21.0.0.

| Platform / compiler | Configuration | Result | CTest elapsed |
|---|---|---|---:|
| macOS / Apple Clang 21, fresh build | Debug | 12/12 passed | 17.27 s |
| macOS / Apple Clang 21, fresh build | Release | 12/12 passed | 3.13 s |
| macOS / Apple Clang 21, fresh build | ASan + UBSan | 12/12 passed, no findings | 14.32 s |
| macOS / Apple Clang 21, fresh build | TSan | 12/12 passed, no findings | 39.10 s |
| Ubuntu 24.04 aarch64 / GCC 13.3 | Debug | 12/12 passed | 15.54 s |
| Ubuntu 24.04 aarch64 / GCC 13.3 | Release | 12/12 passed | 5.35 s |
| Ubuntu 24.04 aarch64 / Clang 18.1.3 | ASan + UBSan + LeakSanitizer | 12/12 passed, no findings | 12.76 s |

The clean Release audit also passed ten consecutive complete 13-scenario TCP
integration runs (6.08 s). The original 67 named cases, 48,000 differential events,
31 allocation-fault points and benchmark-generator checks remain intact. New
coverage includes 33 protocol cases and 12 bounded-queue cases. Live tests verify
correlation, fragmentation/coalescing, size bounds, duplicate requests, disconnects,
resets, streamed output, saturation, slow-reader isolation, concurrent sequencing,
recorded-order replay and shutdown. No known functional defect or confirmed
sanitizer finding remains.

Test logs: each native audit directory’s `Testing/Temporary/LastTest.log`, and
`build/continuation-linux-2026-10-31.log`. The Release log was subsequently used
for stress repetitions. Saved-build checks are additionally recorded in
`build/continuation-native-2026-10-31.log`. These generated logs stay ignored.

## Existing TCP benchmarks rerun

The existing harness ran unchanged, sequentially after tests finished: seed 60448,
2,000 requests/client, three throughput trials, two PING warmups/client, pipeline
eight, and one separate sampled RTT replay per configuration. The fresh Release
audit gateway was selected with `--binary`. Rates include Python/GIL, sockets,
parsing, validation and scheduling; they do not isolate server capacity.

| Workload | Clients | Median requests/s | CV (%) | RTT p50 µs | RTT p95 µs | RTT p99 µs |
|---|---:|---:|---:|---:|---:|---:|
| ping | 1 | 34,712 | 11.44 | 241.7 | 309.5 | 358.4 |
| ping | 4 | 43,546 | 0.65 | 719.4 | 1,012.6 | 1,142.9 |
| ping | 8 | 32,790 | 1.07 | 1,912.0 | 2,861.9 | 3,276.2 |
| ping | 16 | 31,076 | 1.66 | 3,978.2 | 6,088.8 | 7,094.9 |
| matching | 1 | 32,527 | 1.09 | 249.2 | 291.2 | 309.6 |
| matching | 4 | 39,970 | 0.73 | 780.9 | 1,138.2 | 1,310.3 |
| matching | 8 | 31,020 | 0.76 | 2,004.8 | 3,022.5 | 3,561.0 |
| matching | 16 | 30,430 | 1.70 | 4,104.2 | 6,327.3 | 7,335.4 |
| cancellation | 1 | 33,499 | 1.17 | 247.5 | 315.1 | 470.7 |
| cancellation | 4 | 41,265 | 0.65 | 747.8 | 1,066.0 | 1,236.2 |
| cancellation | 8 | 32,063 | 0.91 | 1,931.8 | 2,959.8 | 3,470.5 |
| cancellation | 16 | 31,138 | 0.76 | 3,992.5 | 6,178.2 | 7,235.8 |

All 522,000 measured scaling requests completed with zero overloads,
engine rejections or discarded completions. The maximum server-process RSS
reported across these throughput trials was 4,464 KiB. This is high-water process usage,
including allocator retention and lifetime IDs, not a fixed memory bound or live
heap measurement. The original methodology and caveats remain in
[NETWORK_PERFORMANCE.md](NETWORK_PERFORMANCE.md).

The separate PING overload experiment used request/completion capacity one,
pipeline 64 and pending capacity 64/client, with the same seed, trial count and
warmups. No request was retried.

| Clients | Completed requests | Processed | Explicit overloads | Queue high water | Discarded |
|---|---:|---:|---:|---:|---:|
| 4 | 24,000 | 13,151 | 10,849 | 1 | 0 |
| 16 | 96,000 | 60,798 | 35,202 | 1 | 0 |

All measured requests are accounted for as processed or explicitly overloaded.
Queue admission remains distinct from engine acceptance and response delivery.
No admitted completion was lost in these observed complete-response trials.

## Same-compiler baseline regression control

Commit `b9d0f24` was exported with `git archive` inside the ignored build directory,
without creating a nested Git repository. Its core benchmark and the current
core benchmark were independently compiled with the same updated compiler and
Release settings. Both **complete executable files are byte-for-byte identical**:

```text
SHA-256: 2c6b73b9a654a98f323926ae9707516526685ea557004aaa852885d109f3ad3d
```

Each process used one million events/workload, seven trials, two warmups,
10,000 separate latency samples and reserve zero. Both process orders were
preserved.

| Workload | Baseline median M events/s | Current median M events/s | Initial change | Reverse-order change |
|---|---:|---:|---:|---:|
| resting | 10.924 | 11.133 | +1.92% | -3.51% |
| crossing | 8.960 | 8.751 | -2.33% | -4.33% |
| cancellation | 16.167 | 15.837 | -2.04% | -5.12% |
| deep | 7.963 | 7.828 | -1.70% | -7.92% |
| touch | 12.309 | 11.900 | -3.32% | -2.53% |
| mixed | 14.724 | 14.830 | +0.72% | -2.32% |

Runtime timing differences reached 7.92% across the paired process comparisons,
despite identical executable bytes. They cannot be attributed to gateway code
in the core benchmark. CPU affinity, power state, ASLR, caches and other runtime
conditions are not controlled, and the specific cause was not isolated. No
speedup is claimed. All replay/state hashes, activity and allocation measurements
agree across the four core runs. Source and binary identity confirm that the
optional networking targets add no code or locks to this core benchmark.

## Evidence and readiness

Raw CSVs and the binary-identity record are in
[measurements/audit-2026-10-31](measurements/audit-2026-10-31/). Historical
2026-10-29 evidence is preserved separately. All tests and benchmarks requested
for this continuation completed; no working implementation was replaced.

The project is ready for GitHub review and SWE resume inclusion as a verified
educational systems project. No required local implementation blocker remains.
Hosted GitHub Actions has not run and remains an external verification step
after authorized publication. The historical GCC/Linux sanitizer timeout is
documented; the supported Clang/Linux sanitizer suite passes without reduced
coverage or suppressed findings. This is not a secure public trading endpoint:
TLS, authentication, order ownership and durable recovery remain outside scope.
