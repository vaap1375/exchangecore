#!/bin/sh
# Uses an existing local Docker runtime; never publishes an image or service.
set -eu
cd "$(dirname "$0")/.."
docker build -f scripts/Dockerfile.linux -t exchangecore-toolchain:local .
docker run --rm --network none -v "$PWD:/work" -w /work exchangecore-toolchain:local sh -c '
    cmake -S . -B build/linux-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DEXCHANGECORE_ENABLE_INVARIANTS=ON &&
    cmake --build build/linux-debug --parallel 4 &&
    ctest --test-dir build/linux-debug --output-on-failure &&
    cmake -S . -B build/linux-release -G Ninja -DCMAKE_BUILD_TYPE=Release &&
    cmake --build build/linux-release --parallel 4 &&
    ctest --test-dir build/linux-release --output-on-failure &&
    CC=clang CXX=clang++ cmake -S . -B build/linux-clang-sanitize -G Ninja -DCMAKE_BUILD_TYPE=Debug -DEXCHANGECORE_ENABLE_INVARIANTS=ON -DEXCHANGECORE_SANITIZERS=ON &&
    cmake --build build/linux-clang-sanitize --parallel 4 &&
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build/linux-clang-sanitize --output-on-failure
'
