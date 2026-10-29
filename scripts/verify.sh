#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ -x .venv/bin/cmake ]; then PATH="$PWD/.venv/bin:$PATH"; export PATH; fi
for preset in debug release sanitize; do
    cmake --preset "$preset"
    cmake --build --preset "$preset" --parallel 4
    ctest --preset "$preset"
done
build/release/exchangecore validate examples/orders.csv
build/release/exchangecore validate examples/fifo.csv
build/release/exchangecore validate examples/market.csv
build/release/exchangecore validate examples/mixed.csv
build/release/exchangecore_bench --events 1000 --trials 1 --samples 100 --warmup 0
