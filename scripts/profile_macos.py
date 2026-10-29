#!/usr/bin/env python3
"""Sample a child benchmark process on macOS; outputs stay in build/."""
import pathlib
import shutil
import subprocess
import sys
import time

root = pathlib.Path(__file__).resolve().parent.parent
sample = shutil.which("sample")
if sys.platform != "darwin" or not sample:
    raise SystemExit("This helper requires macOS sample. Use perf on Linux.")
binary = root / "build/release/exchangecore_bench"
if not binary.exists():
    raise SystemExit("Build the release preset first.")
with (root / "build/profile-benchmark.txt").open("w") as output:
    child = subprocess.Popen(
        [str(binary), "--events", "200000", "--trials", "100", "--samples", "1",
         "--warmup", "0", "--workload", "deep"], stdout=output, stderr=subprocess.STDOUT,
        cwd=root,
    )
    try:
        time.sleep(1)
        profile = subprocess.run(
            [sample, str(child.pid), "3", "1", "-file", str(root / "build/profile-sample.txt")],
            capture_output=True, text=True, timeout=15,
        )
        print(profile.stdout, end="")
        print(profile.stderr, end="", file=sys.stderr)
        child.wait(timeout=120)
        if child.returncode:
            raise SystemExit(f"Benchmark failed: {child.returncode}")
        raise SystemExit(profile.returncode)
    finally:
        if child.poll() is None:
            child.terminate()
            child.wait(timeout=10)
