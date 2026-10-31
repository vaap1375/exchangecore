# Implementation status

Checkpoint: TCP upgrade and continuation audit complete, 2026-10-31.

## Completed continuation audit

The existing networking upgrade was inspected and preserved. No production
source change was needed. HEAD remains `b9d0f24`; gateway changes remain
uncommitted, with no remote or publication. Original matching/API/benchmark
source comparisons to the baseline pass.

Clean builds with the updated Apple Clang 21.0.0 (clang-2100.4.1.1) passed all
12 CTest groups in Debug, Release, ASan+UBSan and TSan, with no sanitizer
findings. Linux GCC 13 Debug/Release and Clang 18 ASan+UBSan+LeakSanitizer also
passed all 12 groups. Ten complete 13-scenario TCP stress repetitions passed.

The existing core and TCP benchmarks were rerun sequentially, with new evidence
preserved separately. Four-client matching measured 39,970 complete TCP
requests/s median, including Python client costs. All 522,000 scaling requests
completed without overload or engine rejection. Capacity-one overload trials
accounted for all 120,000 requests as processed or explicitly overloaded.

The baseline was exported without Git metadata inside the ignored build tree.
Independently compiled baseline/current core benchmark executables are
byte-for-byte identical. Both run orders and their timing variation are retained;
no gateway overhead or core optimization is inferred from those differences.

See [the dated audit](VERIFICATION_2026-10-31.md) for current platform timings,
benchmark tables, binary identity, raw data and limitations. No required local
implementation blocker remains. Hosted CI is configured but unrun; the earlier
GCC/Linux sanitizer timeout remains documented, with Clang coverage passing.

The following sections record the completed 2026-10-29 implementation evidence.

The original baseline commit is `b9d0f24` (created by the owner before this
upgrade). This session created no commits, remotes, nested repository, pushes
or publication. All changes are local and reviewable. The original matching
engine, its public API and core benchmark source remain unchanged.

## Completed milestones

- Verified original Release baseline: 8/8 CTest groups and fresh million-event
  measurements before integration.
- Added optional POSIX TCP executable, nonblocking poll I/O, RAII descriptors,
  dedicated matching writer and bounded request/completion queues.
- Implemented EC1 LIMIT, MARKET, CANCEL, BEST and PING with request correlation,
  admission sequences, strict framing and duplicate/retry rules.
- Added bounded input/output/pending resources, explicit queue overload,
  incremental multi-trade streaming and generation-safe response routing.
- Implemented disconnect/half-close/reset handling and graceful accepted-work
  draining with a response grace period.
- Added client utility, automatic two-client demo, independent integration tests,
  queue/protocol unit tests, repeated stress and separate TCP benchmarks.
- Validated macOS and Linux; added sanitizer configuration, CI updates and local
  Linux verification tooling. CI YAML lint passed; hosted jobs remain unrun.
- Recorded actual core before/after, TCP scaling, overload, latency, queue and
  memory measurements. Preserved all original performance evidence.
- Completed architecture, protocol, concurrency, testing, performance and resume
  documentation and final technical/hygiene review.

## Final verification

All configurations below passed **12/12 CTest groups**, zero test failures or
sanitizer findings in those final runs:

| Platform / compiler | Configuration | Elapsed |
|---|---|---:|
| macOS, Apple Clang 21 | Debug, automatic invariants | 17.31 s |
| macOS, Apple Clang 21 | Release | 2.53 s |
| macOS, Apple Clang 21 | ASan + UBSan, -O1 | 14.95 s |
| macOS, Apple Clang 21 | ThreadSanitizer, -O1 | 39.61 s |
| Ubuntu 24.04 aarch64 container, GCC 13.3 | Debug | 14.91 s |
| Ubuntu 24.04 aarch64 container, GCC 13.3 | Release | 5.12 s |
| Ubuntu 24.04 aarch64 container, Clang 18.1.3 | ASan + UBSan + LeakSanitizer, -O1 | 11.38 s |

The original eight groups remain intact: 67 named core/parser/reference cases,
48,000 differential events, seven allocation scenarios / 31 fault points,
594 generator combinations and 22 CLI process checks. New groups cover 33 named
protocol cases, 12 queue cases (including concurrent exactly-once delivery),
13 live TCP integration scenarios and the two-client demo.

The complete 13-case integration suite also passed ten consecutive Release
repetitions (6.64 s). After final client-boundary hardening, Release integration
and demo passed again (1.09 s); the standalone client utility was separately
verified against an ephemeral server. The gateway-off build passed the original
8/8 groups (2.21 s), confirming optionality.

Clang static analysis of new protocol, gateway and gateway-main sources emitted
no findings. GitHub Actions YAML passed local actionlint. Original core sources
and benchmark source compare byte-for-byte with the baseline commit.

Local log evidence (ignored generated files):

- `build/network-baseline/ctest.log`
- `build/network-final.log` (all macOS presets and ten stress repetitions)
- `build/network-client-final.log`, `build/core-only-verification.log`
- `build/linux-clang-final.log` (GCC Debug/Release, Clang sanitizers)
- `build/gateway-static-analysis.log`

## Fixed failures and limitations

A rapid reset reproduced macOS SO_NOSIGPIPE returning EINVAL during accepted
socket setup. This originally stopped the listener; now only that accepted
connection closes. Tests retain reset/reconnect coverage. Review also hardened
POLLHUP handling to avoid idle-descriptor spinning, final completion-discard
accounting, client input bounds and socket setup cleanup.

GCC 13 Linux sanitizer runs, at both -O0 and -O1, exceeded reference-test (180s)
and CLI benchmark-smoke (30s) deadlines without a sanitizer finding. Probes
ruled out VM memory pressure; allocation-stack-depth and quarantine experiments
did not resolve it and were not retained. The exact compiler/runtime cause is
unproven. Clang 18 passes all unchanged tests with all checks and leak detection
enabled, so local Linux verification and hosted Linux sanitizer configuration
select Clang. GCC sanitizer runs are not reported as passes. Their diagnostic
logs remain in `build/linux-final.log` and `build/linux-upgrade.log`.

The first sandboxed host loopback attempts failed at bind; scoped local network
access succeeded. Linux containers used isolated loopback with no published
ports. No Linux performance or Linux TSan measurement is claimed. Hosted CI is
configured and linted but has not run; no publication was authorized.

No known functional bugs or confirmed sanitizer findings remain. No required
local implementation work is incomplete or blocked. The GCC sanitizer performance
limitation is documented rather than hidden.

## Architecture decisions

- One I/O owner owns all sockets and bounded buffers; one matching owner owns
  the original engine. No locks enter the core matching path.
- Fixed-slot FIFO queues use mutexes/condition variables; matching and socket I/O
  execute outside queue locks. Closing queues wakes waiters and drains work.
- Queue admission assigns a sequence. Engine acceptance is a later result.
  Concurrent arrival order is not repeatable; replay of the recorded chosen
  sequence is tested against complete results and BEST observations.
- Connection generations, not raw descriptor references, route completions.
  Disconnected accepted work still executes; responses can become undeliverable.
- Strictly increasing connection request IDs prevent repeated execution of the
  same valid request. Global lifetime order IDs protect retries across reconnects.
- Response slots preserve per-connection order and stream bounded lines. Pending
  and output limits pause reads; full request queues explicitly reject overload.
- SIGINT/SIGTERM use an always-lock-free atomic stop flag. Matching drains before
  the two-second default response grace begins. Slow readers cannot block joining.

Defaults: loopback IPv4:9000, clients 128, request/completion queues 1024 each,
pending 16/client, input 4096 bytes plus detection byte, output 65,536 bytes/client,
lifetime accepted new IDs 1M. These are finite configured bounds, not a promise
of a small fixed RSS: core nodes, accepted-ID history and trade vectors allocate.

## Verified performance

Measured Apple M1 Max (10 cores, 32 GB), macOS 26.6.2, Apple Clang 21 Release. Full
methodology, min/max/CV, RTT and memory caveats: `NETWORK_PERFORMANCE.md`.

Original core workload: one million events, seed 60448, seven trials, two warmups,
10,000 separate latency samples, reserve=0. Fresh median changes before→after:

| Workload | Before M events/s | After M events/s | Change |
|---|---:|---:|---:|
| Resting | 10.760 | 10.936 | +1.63% |
| Crossing | 8.786 | 8.774 | -0.14% |
| Cancellation | 15.937 | 15.998 | +0.39% |
| Deep | 7.597 | 7.790 | +2.55% |
| Touch | 12.158 | 12.184 | +0.21% |
| Mixed | 14.703 | 14.972 | +1.83% |

No material median regression observed. Resting post-run CV 11.26% shows noise;
these changes are not attributed to a core optimization. Matching activity,
allocation counts and replay/state hashes agree. The core source did not change.

TCP scaling: three workloads × clients 1/4/8/16 × three throughput trials,
2,000 requests/client, pipeline 8, separate sampled RTT replay. Across 522,000
throughput-trial requests there were zero overloads, engine rejections or
unroutable completions. Four-client matching median 39,954 completed requests/s;
p50/p95/p99 sampled RTT 800.1/1135.0/1307.6 microseconds. These include Python GIL,
client parsing, sockets, queues and server processing, not server capacity alone.

Separate queue=1/completion=1/pipeline=64 PING overload trials completed 120,000
requests: 4 clients 10,479/24,000 overload responses; 16 clients 33,550/96,000. Queue
high water stayed 1; every observed request is accounted for as processed or
explicitly overloaded. There were no retries or discarded completions.

Scaling server RSS startup 1648 KiB; process peak across throughput servers ranged
1856–4464 KiB. High-water growth includes retained lifetime IDs and allocator
state and is not a live-heap/leak measurement. The measured maximum normal
queue depth was 27; largest per-connection output length 1123 bytes under the 65,536-byte cap.

Raw evidence: `docs/measurements/core-before-network.csv`,
`core-after-network.csv`, `network-scaling.csv`, `network-overload.csv`.

## Resume and remaining work

Ready for local technical review and resume discussion. No required local tasks
remain. Hosted CI must run after any future authorized publication; production
hardening is outside scope. No TLS/authentication, per-client order ownership,
durable recovery, maker push notifications, IPv6 or real trading is implemented.

Resume commands and publication constraints are in `NEXT_STEPS.md`.
