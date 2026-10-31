# Continuation checkpoint

The existing TCP upgrade and the 2026-10-31 continuation audit are complete.
No production source changes were needed during the audit. Do not rebuild the
matching core or restart completed work. Read
[the dated verification report](VERIFICATION_2026-10-31.md),
`IMPLEMENTATION_STATUS.md`, README and Git status before new changes.

Fresh native Debug/Release/ASan+UBSan/TSan builds, Linux checks, ten complete TCP
stress repetitions and all existing core/network benchmarks passed. New raw
measurements are preserved under `docs/measurements/audit-2026-10-31/` alongside
the historical evidence. Baseline/current core benchmark executable bytes match;
process timing variation is documented without a speedup claim.

The owner-created original baseline is `b9d0f24`. This session made no commits,
remotes, pushes or publication. Those actions remain unauthorized. Work only in
this repository; do not create a nested Git repository.

## Verified state

- macOS Debug/Release/ASan+UBSan/TSan: 12/12 CTest groups each.
- Linux GCC 13 Debug/Release and Clang 18 ASan+UBSan+LSan: 12/12 each.
- Original 67 named cases/48,000 differential events/31 fault points preserved;
  new 33 protocol/12 queue cases and 13 live network integration scenarios.
- Ten repeated complete network suites passed; standalone client/demo verified.
- Gateway-off 8/8 passed; core/API/benchmark source unchanged.
- Actual core before/after and TCP scaling/overload CSVs are preserved under
  docs/measurements, separately from original historical core evidence.
- Static analysis and CI YAML lint passed; hosted CI remains unrun.

No required local task is incomplete. If new work is requested, implement only
that scope and run affected checks. Preserve labeled original measurements.

## Commands

```sh
export PATH="$PWD/.venv/bin:$PATH"
cmake --preset release
cmake --build --preset release

# Two clients, fills, queries, cancellation, exact routing and automatic shutdown:
python3 scripts/network_demo.py

# Standalone localhost server; use another terminal for the client:
build/release/exchangecore_gateway --host 127.0.0.1 --port 9000
python3 scripts/network_client.py --port 9000 --command 'LIMIT 1 SELL 10 24001' --command BEST

# All native presets plus ten repeated network suites (loopback access needed):
scripts/verify_network.sh

# Existing local Docker daemon; compiler image stays local, no published ports:
scripts/verify_linux.sh

# Separate network measurement (full labels/method in NETWORK_PERFORMANCE.md):
python3 benchmarks/network_benchmark.py --clients 1,4,8,16 --workloads ping,matching,cancellation --requests-per-client 2000 --trials 3 --pipeline 8

# Original in-process benchmark remains separate:
build/release/exchangecore_bench --events 1000000 --trials 7 --warmup 2 --samples 10000
```

GCC 13 Linux sanitizer tests were too slow under this toolchain (reference 180s,
CLI smoke 30s), with no sanitizer finding. Do not mark them passed. Default
`verify_linux.sh` uses Clang 18 for sanitizers and passes unchanged coverage with
leak detection enabled. Diagnostic logs: build/linux-final.log; passing logs:
build/linux-clang-final.log. Do not suppress sanitizer errors or shrink tests to
hide a failure.

The managed host sandbox may deny loopback bind. Use scoped permission for local
tests; no privileged port or public endpoint is needed. The Docker test runtime
was already installed; the toolchain image is only a local validation fixture.
Ignored local artifacts: .venv/, build/, benchmark-results/, __pycache__/ and
failure artifacts. No background test gateway should remain running.
