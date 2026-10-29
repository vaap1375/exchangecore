#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ -x .venv/bin/cmake ]; then PATH="$PWD/.venv/bin:$PATH"; export PATH; fi
cmake --preset release
cmake --build --preset release --parallel 4
mkdir -p benchmark-results
task_events=${1:-100000}
# Sequential runs avoid concurrent engine workloads contaminating measurements.
build/release/exchangecore_bench --events "$task_events" --trials 7 --warmup 2 --samples 10000 --seed 60448 --reserve 0 --csv > benchmark-results/baseline.csv
build/release/exchangecore_bench --events "$task_events" --trials 7 --warmup 2 --samples 10000 --seed 60448 --reserve "$task_events" --csv > benchmark-results/reserved.csv
echo 'Results: benchmark-results/baseline.csv and benchmark-results/reserved.csv'
