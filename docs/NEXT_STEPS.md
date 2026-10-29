# Session continuation

**The requested local project is complete and verified as of 2026-10-29.**
Do not restart implementation. Read `IMPLEMENTATION_STATUS.md`, inspect files
and `git status --short` before making changes. Preserve completed work and raw
measurement records.

The repository must remain local until the owner gives new authorization:
no nested Git initialization, remotes, commits, pushes or publishing.

## Current checkpoint

- All 8/8 CTest groups pass in Debug, Release and ASan/UBSan.
- 67 named cases, 48,000 differential events, 31 allocation fault points,
  594 benchmark generator combinations, 22 CLI process checks.
- All four CSV examples validate; standalone benchmark smoke passes.
- Static analysis and isolated installed-library consumer passed.
- 100k and 1M baseline/reserved measurements repeated in both process orders;
  raw CSVs and profiler excerpt are in `docs/measurements`.
- README and all engineering/performance/resume documents are complete.
- Independent final code/doc reviews found no material outstanding issue.
- No known bugs, failing tests or locally blocked required work.

## What a future session should do

1. Confirm the user's new objective and read the current status; avoid adding
   unrelated features merely because implementation is complete.
2. If source or environment changes, run the appropriate verification below.
3. If publishing is newly authorized, inspect all files and license preferences,
   then run hosted CI. Do not claim Linux or hosted CI success in advance.
4. For performance work, preserve baseline `reserve=0`, isolate a controlled change,
   check correctness, repeat both run orders, and report variation and memory
   costs. Original measurements must stay labeled with their hardware/date.

## Commands

```sh
# Only if local build tooling is absent:
python3 -m venv .venv
.venv/bin/python -m pip install cmake ninja
export PATH="$PWD/.venv/bin:$PATH"

# Full verification (about a minute on the original machine):
scripts/verify.sh

# Recruiter demonstration and replay:
build/release/exchangecore demo
build/release/exchangecore replay examples/orders.csv

# Reproduce both configurations:
scripts/benchmark.sh 1000000
python3 scripts/compare_benchmarks.py benchmark-results/baseline.csv benchmark-results/reserved.csv

# macOS profiling; OS permission may be needed for the spawned child:
python3 scripts/profile_macos.py
```

Ignored local artifacts: `.venv/`, `build/` (verification.log, CTest logs, profile,
installed-consumer checks), `benchmark-results/`, `test-failures/` on failure.
Checked-in-source candidates: everything outside those ignored directories,
including labeled raw measurements under docs. No commit exists yet.
