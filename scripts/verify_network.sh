#!/bin/sh
# Local loopback tests need permission to bind ephemeral localhost ports.
set -eu
cd "$(dirname "$0")/.."
if [ -x .venv/bin/cmake ]; then PATH="$PWD/.venv/bin:$PATH"; export PATH; fi
for preset in debug release sanitize thread-sanitize; do
    cmake --preset "$preset"
    cmake --build --preset "$preset" --parallel 4
    ctest --preset "$preset"
done
ctest --preset release -R network_integration --repeat until-fail:10
