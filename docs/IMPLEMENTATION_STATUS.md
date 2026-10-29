# Implementation status

Last checkpoint: final technical review complete, 2026-10-29.

**Overall: all locally achievable required milestones completed and verified.**
The existing Git repository was initially empty. It remains uncommitted on main,
with no remote, push or publishing. No nested repository was created.

## Completed milestones

1. Inspected repository, preserved existing Git metadata, installed local build
   tools in `.venv` and selected portable C++20/CMake architecture.
2. Built reusable matching library, explicit typed results/rejections, lifetime
   ID policy, exact ticks/quantity arithmetic, cancellation and read-only snapshots.
3. Implemented bounded CSV parsing, deterministic replay/validation, CLI demo,
   exact price formatting and four example workloads.
4. Added invariant checks, independent vector/scan reference, meaningful boundary
   tests, allocation-failure rollback tests and black-box CLI contracts.
5. Compiled and tested Debug, Release and combined ASan/UBSan configurations.
6. Measured six seeded workloads with independent throughput, latency, allocation
   and RSS passes; evaluated configurable hash reservation against baseline.
7. Profiled a local Release child with macOS sample and preserved evidence.
8. Completed architecture, matching, complexity, performance, testing and resume
   documentation; configured Linux/macOS hosted CI without claiming it has run.
9. Performed independent core/parser/build/documentation reviews, Clang static
   analysis, installed-consumer test and repository hygiene checks.

## Final verification

`scripts/verify.sh` passed all **8/8 CTest groups**, zero failures in each preset:

| Configuration | CTest elapsed | Result |
|---|---:|---|
| Debug, automatic invariants | 15.25 s | Passed |
| Release, automatic invariants disabled | 1.58 s | Passed |
| AddressSanitizer + UndefinedBehaviorSanitizer | 45.51 s | Passed, no sanitizer diagnostics |

Within those groups:

- 67 named cases: 43 core, eight differential regimes, 15 CSV, one price formatter.
- 48,000 reference-comparison events (eight regimes, four seeds, 1,500 events each).
- Seven allocation-failure scenarios, 31 injected allocation sites, rollback and retry.
- Four generator groups, including 594 seed/count combinations compared to reference.
- 22 black-box CLI checks, including deterministic stdout equality and error exits.
- Four example files validated; all six standalone benchmark smoke workloads passed.

Clang static analysis of `order_book.cpp`, `matching_engine.cpp`, `csv.cpp` and
`price.cpp` emitted no findings. A separate installed find_package consumer built
and ran using engine, parser and price APIs. Source/doc whitespace and personal-path
checks passed; all eight raw CSV files agree in their activity and replay/state
checksums within each event-count configuration.

Earlier test failures (benchmark invalid-option exit code and macOS directory
input handling) were fixed with regression checks. Review also fixed parser
failbit handling, physical error line numbers and the diagnostic cap. Final runs
above supersede those earlier failures. **No known bugs or failing tests remain.**

## Architecture decisions

- Single serialized writer: deterministic sequence assignment without locking.
- Ordered price maps, per-level FIFO lists, iterator-valued active hash index,
  separate lifetime-ID set; all ownership uses RAII containers.
- Prepare executions and stage allocations before committing fills, preserving
  semantic state on bad_alloc; no full-book cloning or custom allocator.
- Positive integer ticks and quantities, conservative aggregate overflow guard,
  configured lifetime capacity (default one million new accepted IDs).
- Snapshots return detached values; no borrowed mutable iterators.
- Hash reservation remains optional and baseline remains available as `reserve=0`.

## Verified measurements

Hardware: Apple M1 Max, 10 cores (8 performance / 2 efficiency), 32 GB RAM;
macOS 26.6.2 / Darwin 25.6.0 arm64. Apple Clang 21.0.0, CMake 4.4.4, Ninja 1.13.2.

At one million events/workload, seed 60448, seven throughput trials, two warmups,
10,000 separate latency samples, first baseline medians (million input events/s):
resting 11.179, crossing 8.913, cancellation 16.202, deep 7.833, touch 12.255,
mixed 14.520. Reverse-order mixed baseline 14.356 M/s. Full min/max/CV and latency
statistics are in `PERFORMANCE.md`; raw CSVs are in `docs/measurements`.

Configurable reservation improved million-event deep-book medians 6.18% and 11.60%
in the two run orders; resting 8.71% and 11.43%. Some reverse trials had substantial
variation. Initial 100k improvements mostly failed to repeat. Reservation removes
rehash requests but has upfront memory costs and can increase gross requests on
short-lived workloads. No universal speedup or end-to-end latency claim is made.

macOS sample captured 2,464 main-thread stacks over three seconds. Sampling
includes validation/snapshots/teardown as well as matching; the report does not
mislabel all samples as timed matching work. Sandbox access escalation for our
own benchmark child succeeded. Raw profile remains ignored under build/.

## Active / remaining work

No active required implementation milestone. No locally blocked work.

Completed but not externally verified: hosted GitHub Actions on Linux/macOS,
Linux runtime/sanitizers/LeakSanitizer and Linux perf. No remote or publishing was
authorized, so these results cannot be claimed. Optional future work includes
replace semantics, durable journaling and evidence-driven memory-layout changes;
these are outside the required core and are not represented as complete.

## Exact resume commands

Read `docs/NEXT_STEPS.md`, this document and `git status --short` first.

```sh
export PATH="$PWD/.venv/bin:$PATH"
scripts/verify.sh
build/release/exchangecore demo
scripts/benchmark.sh 1000000
python3 scripts/compare_benchmarks.py benchmark-results/baseline.csv benchmark-results/reserved.csv
```

Rerun checks when code/environment changes justify it. Do not rebuild completed
functionality or overwrite recorded evidence with new numbers without labeling
new runs. Do not commit, create a remote, push or publish without authorization.
