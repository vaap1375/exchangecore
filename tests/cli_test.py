"""Black-box CLI contract tests, without third-party dependencies."""
import pathlib
import subprocess
import sys
import tempfile

exe = sys.argv[1]
checks = 0


def run(args, code=0, contains=None):
    global checks
    p = subprocess.run([exe, *args], capture_output=True, text=True, timeout=30)
    assert p.returncode == code, (args, p.returncode, p.stdout, p.stderr)
    if contains:
        assert contains in p.stdout + p.stderr, (args, p.stdout, p.stderr)
    checks += 1
    return p


run(["--help"], contains="price-time")
run(["demo"], contains="TRADE maker=1 taker=5 quantity=6")
run(["nonsense"], 2)
run(["demo", "extra"], 2)
run(["replay", "definitely-missing-exchangecore.csv"], 1)
with tempfile.TemporaryDirectory(prefix="cli-test-", dir=pathlib.Path.cwd()) as directory:
    run(["replay", directory], 1)
    f = pathlib.Path(directory) / "events.csv"
    f.write_text("type,id,side,quantity,price\nLIMIT,1,SELL,10,24001\nMARKET,2,BUY,4,\nCANCEL,1,,,\n")
    first = run(["replay", str(f)], contains="accepted=3 rejected=0 trades=1")
    second = run(["replay", str(f)])
    assert first.stdout == second.stdout
    run(["validate", str(f)], contains="accepted=3")
    quiet = run(["replay", str(f), "--quiet"])
    assert "TRADE" not in quiet.stdout
    f.write_text("type,id,side,quantity,price\nLIMIT,1,BUY,1,2\nLIMIT,1,BUY,1,2\n")
    run(["validate", str(f)], 2, contains=f"{f}:3: duplicate ID")
    f.write_text("type,id,side,quantity,price\nLIMIT,1,BUY,1,6000.25\n")
    run(["replay", str(f)], 2, contains=f"{f}:2:")
    f.write_text("")
    run(["validate", str(f)], contains="events=0")
    f.write_text("bad,header\n")
    run(["validate", str(f)], 2, contains=f"{f}:1:")
    f.write_text("type,id,side,quantity,price\n" + "x" * 5000 + "\n")
    run(["validate", str(f)], 2, contains="line exceeds")
    f.write_text("type,id,side,quantity,price\nCANCEL,99,,,\n")
    run(["replay", str(f)], 2, contains="unknown active order")

run(["benchmark", "--events", "1000", "--trials", "1", "--warmup", "0", "--samples", "100"])
run(["benchmark", "--events", "0"], 2)
run(["benchmark", "--events", "-1"], 2)
run(["benchmark", "--trials"], 2)
run(["benchmark", "--workload", "invalid"], 2)
run(["benchmark", "--events", "18446744073709551616"], 2)
print(f"{checks} CLI checks passed; deterministic replay output identical")
