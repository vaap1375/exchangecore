# TCP gateway performance evidence

The [2026-10-31 continuation audit](VERIFICATION_2026-10-31.md) reran this
unchanged harness after a compiler update. Its separate raw data includes all
client/workload combinations, overload accounting and same-compiler core
baseline controls. The original measurements below remain preserved.

Measured locally on **2026-10-29**. All reported Release trials passed the
harness's response-correlation, quantity, final-book and shutdown-accounting
checks. The original in-process evidence remains in
[PERFORMANCE.md](PERFORMANCE.md). These are loopback measurements from a Python
client harness, not isolated server-capacity or real-exchange latency claims.

## Scope and environment

The network harness measures end-to-end EC1 requests over loopback, using the
optional `exchangecore_gateway` and Python clients in the same machine. It
includes Python thread scheduling and the GIL, command framing, socket calls,
server queuing/processing, response transmission, parsing and validation. It
does not isolate the gateway's maximum server-side capacity. Core events/s and
complete TCP requests/s are different measurements.

Measured configuration:

- Apple M1 Max, 10 cores, 32 GB RAM; macOS 26.6.2 arm64, Python 3.14.7.
- Apple Clang 21.0.0, CMake Release build.
- Loopback `127.0.0.1`; ephemeral ports; TCP_NODELAY on client connections.
- Seed 60448; 1, 4, 8 and 16 concurrent connections.
- 2,000 measured requests per client, three fresh-server throughput trials,
  two untimed PING warmups per client and pipeline depth eight per client.
- One additional fresh-server RTT replay per workload/client-count pair, sampling
  up to 10,000 requests. CPU affinity, power state and background load are uncontrolled.

The label arguments record the operator's environment description. They do not
inspect compiler flags or verify hardware automatically. Keep the compiler/build
labels aligned with the executable being measured. The CSV also records the
actual platform string, hardware-thread count, Python version and binary path.

## Reproduce the scaling experiment

Build the Release gateway and run the harness from the repository root:

```sh
cmake --preset release
cmake --build --preset release
mkdir -p benchmark-results
python3 benchmarks/network_benchmark.py --clients 1,4,8,16 --workloads ping,matching,cancellation --requests-per-client 2000 --trials 3 --pipeline 8 --latency-samples 10000 --seed 60448 --hardware-label 'Apple M1 Max (10 cores), 32 GB' --compiler-label 'Apple Clang 21.0.0' --build-label Release --csv > benchmark-results/network-scaling.csv
```

Each throughput and RTT trial launches a new server, waits for its flushed
LISTEN readiness message, connects every client and finishes warmup before the
timed start barrier. No fixed startup sleep determines readiness. Graceful
shutdown is checked after all replies and the final book query finish. Bounded
diagnostic readers drain server stdout/stderr throughout the run.

Validated raw results are preserved as
[network-scaling.csv](measurements/network-scaling.csv) and
[network-overload.csv](measurements/network-overload.csv). Preserve separate runs
rather than overwriting evidence, especially when changing the pipeline, queue
capacity, binary or environment.

## Deterministic inputs and correctness checks

| Workload | Input requests | Expected overload-free result |
| --- | --- | --- |
| `ping` | PING requests through the same queue/writer path | One sequenced PONG per request; empty book. |
| `matching` | Alternating SELL limits at 24000 ticks and BUY markets with equal seeded quantities | Markets fully fill; submitted sell quantity is fully consumed; empty book. |
| `cancellation` | Alternating noncrossing BUY limits and cancellation of their IDs | Every cancellation succeeds; no trades; empty book. |

Each client uses a separate order-ID range and strictly increasing request IDs.
The matching workload may execute against another client's earlier sell; its
total quantity accounting remains valid because each client submits a sell
before its corresponding buy. Actual maker IDs, execution counts and global
processing sequences can vary with concurrent admission order. Seeded inputs
do not make socket scheduling deterministic.

The harness correlates every response, requires a complete END boundary and
checks positive processing-sequence uniqueness across measured clients. It
validates result order IDs, trade counts, prices and quantities. In an
overload-free trial it also requires expected fills/cancellations and an empty
final book. After shutdown it compares server admission/processing/overload
counters with observed replies, checks configured queue bounds, and requires
zero discarded completions and zero parser errors for generated requests.

These gates run in addition to the dedicated integration tests. Per-request
response validation remains inside the client throughput path, so its cost is
part of this end-to-end measurement.

## Throughput and RTT boundaries

The throughput interval starts after every connection and warmup is ready,
immediately before releasing clients. It ends at the last client's completed
response. It includes client wakeup/scheduling, bounded pipeline management,
socket I/O, decoding, correlation, quantity checks and sequence bookkeeping.
Workload generation, connections, warmup, final BEST verification and process
shutdown are outside that interval. Each client keeps at most the configured
pipeline count outstanding and replenishes credit as replies complete.

The CSV reports minimum, median and maximum completed requests/s and population
coefficient of variation across three throughput trials. It also reports median
**processed requests/s**, counting positive-sequence replies. Processed includes
engine REJECT outcomes; it is queue/writer activity rather than successful order
acceptance. **Completed requests/s** additionally includes sequence-zero overload
responses. Neither rate should be labeled executed trades/s.

RTT statistics come from a separate fresh-server replay. A seeded selection of
request indices is chosen before timing, distributed across clients. Each sample
starts immediately before sending its request and ends after receiving and
decoding its complete END-terminated reply. Outstanding pipeline work, response
bytes, client scheduling and parsing all contribute to RTT. The reported average
is the sample arithmetic mean; p50, p95 and p99 use nearest rank. Sample count
clamps to the trial's total request count.

Sampling adds clock/branch overhead, and a single RTT replay does not establish
a service-level tail-latency guarantee. Throughput and RTT have separate timing
boundaries and are not reciprocals. The Python harness can be a bottleneck;
increasing clients need not increase throughput even when the server has spare
CPU capacity.

## Scaling results

Throughput units below are thousands of complete requests/s; CV is percent.
Overloads are summed across the three throughput trials.

| Workload | Clients | Minimum | Median | Maximum | CV (%) | Median processed/s (thousands) | Overloads |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| ping | 1 | 28.656 | 35.858 | 35.921 | 10.19 | 35.858 | 0 |
| ping | 4 | 42.608 | 42.637 | 44.103 | 1.62 | 42.637 | 0 |
| ping | 8 | 31.247 | 31.297 | 31.449 | 0.27 | 31.297 | 0 |
| ping | 16 | 30.066 | 30.121 | 30.337 | 0.39 | 30.121 | 0 |
| matching | 1 | 34.015 | 34.125 | 34.218 | 0.24 | 34.125 | 0 |
| matching | 4 | 39.374 | 39.954 | 40.305 | 0.96 | 39.954 | 0 |
| matching | 8 | 29.487 | 30.091 | 30.421 | 1.29 | 30.091 | 0 |
| matching | 16 | 29.382 | 29.530 | 29.744 | 0.50 | 29.530 | 0 |
| cancellation | 1 | 32.380 | 33.555 | 34.513 | 2.61 | 33.555 | 0 |
| cancellation | 4 | 41.541 | 41.613 | 41.699 | 0.15 | 41.613 | 0 |
| cancellation | 8 | 30.642 | 30.977 | 31.234 | 0.78 | 30.977 | 0 |
| cancellation | 16 | 30.232 | 30.752 | 30.974 | 1.01 | 30.752 | 0 |

Separate sampled RTT, in microseconds:

| Workload | Clients | Samples | Average | p50 | p95 | p99 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| ping | 1 | 2000 | 223.9 | 222.4 | 254.9 | 284.0 |
| ping | 4 | 8000 | 736.6 | 731.6 | 1004.6 | 1170.4 |
| ping | 8 | 10000 | 2026.1 | 1991.0 | 2948.6 | 3365.6 |
| ping | 16 | 10000 | 4214.2 | 4131.4 | 6346.8 | 7460.1 |
| matching | 1 | 2000 | 244.1 | 239.3 | 294.0 | 302.2 |
| matching | 4 | 8000 | 811.3 | 800.1 | 1135.0 | 1307.6 |
| matching | 8 | 10000 | 2096.8 | 2067.4 | 3116.6 | 3600.9 |
| matching | 16 | 10000 | 4275.9 | 4174.8 | 6532.6 | 7638.7 |
| cancellation | 1 | 2000 | 231.8 | 229.8 | 264.0 | 288.9 |
| cancellation | 4 | 8000 | 778.8 | 765.4 | 1080.5 | 1290.2 |
| cancellation | 8 | 10000 | 2096.5 | 2008.0 | 3141.8 | 4892.1 |
| cancellation | 16 | 10000 | 4173.8 | 4082.5 | 6331.5 | 7314.3 |

The 12 configurations completed **522000 measured throughput requests** with
zero overloads, engine rejections or discarded completions. Median rates ranged
from 29.530k to 42.637k requests/s. All three workloads had their highest observed
median at four clients; throughput fell at eight and sixteen. The Python GIL,
client validation, server work and OS scheduling all contribute, so these runs
do not identify a server-only scaling limit.

A representative matching configuration with four clients and pipeline eight
measured **39953.6 requests/s median** (CV 0.96%), with a separate sampled RTT
p50 of 800.1 us, p95 of 1135.0 us and p99 of 1307.6 us. Single-client PING had
CV 10.19%, including a lower 28.656k requests/s trial; keep that variation when
interpreting its median. Do not extrapolate these loopback requests to external
network latency, real exchange traffic or the in-process millions-of-events/s
results.

## Deliberate overload experiment

Run a distinct saturation configuration using only PING, so rejected requests
cannot alter subsequent order/cancellation preconditions:

```sh
python3 benchmarks/network_benchmark.py --clients 4,16 --workloads ping --requests-per-client 2000 --trials 3 --pipeline 64 --queue-capacity 1 --completion-capacity 1 --pending-per-client 64 --warmup-requests 2 --latency-samples 10000 --seed 60448 --hardware-label 'Apple M1 Max (10 cores), 32 GB' --compiler-label 'Apple Clang 21.0.0' --build-label Release --csv > benchmark-results/network-overload.csv
```

Every measured request is sent once. OVERLOADED is an explicit completion with
sequence zero, rather than a successful admission; the harness never retries it.
Queue/writer rates and rejected counts are therefore reported separately from
total completed-response rates. An overloaded syntactically valid request still
consumes its request ID. An application choosing to retry must use a higher ID;
the protocol's disconnect/ambiguity contract remains unchanged.

| Clients | Completed median kreq/s | Processed median kreq/s | Completed responses | Processed responses | Overloaded responses | Queue high water | Discarded completions |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 4 | 44.438 | 24.871 | 24000 | 13521 | 10479 | 1 | 0 |
| 16 | 33.495 | 21.831 | 96000 | 62450 | 33550 | 1 | 0 |

The 4-client run explicitly rejected 10479 of 24000 measured requests (43.66%);
the 16-client run rejected 33550 of 96000 (34.95%). Every measured response was
accounted for as processed or overloaded, and no admitted completion was lost.
Both request and completion FIFO high waters remained at their configured
capacity of one. Completed rates include these inexpensive rejection replies;
they are not successful-processing throughput.

The separate RTT replays observed 3838 overloads among 8000 total requests and
10901 among 32000 respectively. Their p50 RTTs were 5464.3 us and 30520.9 us;
p99 values were 8142.8 us and 38279.6 us. The larger pipeline and rejected-response
mix make these a separate experiment from the pipeline-eight scaling table.

The separate RTT replay can have a different overload count because scheduling
varies. Its percentile samples include both processed and overloaded replies;
`latency_overloaded` records that replay's complete overload count. If saturation
is run with matching/cancellation instead, overload can legitimately cause later
markets to remain unfilled or cancellations to reject; the harness records
those outcomes without silently retrying them.

## Memory and bounded occupancy

The gateway reports `startup_rss_kib` and `peak_rss_kib` using its own process's
`getrusage` high-water measurement, converting macOS bytes to KiB; Linux already
reports KiB. CSV rows summarize minimum/maximum across throughput-trial servers
and record the separate RTT server's peak. `server_high_water_growth_kib_max`
is the maximum within-trial peak-minus-startup difference. It includes book/ID
storage, queues, connections, buffers, allocator retention and shutdown work;
it is neither precise live heap growth nor proof of a memory leak.

`server_rss_available` distinguishes optional server RSS reporting from zero
placeholders. The legacy `children_lifetime_peak_rss_kib` is a separate
Python-process-lifetime high water across reaped children; it cannot identify
one trial's gateway usage and should not replace server-specific fields.

The CSV also records maximum request/completion FIFO occupancy and the largest
logical per-connection output-buffer occupancy. Server STATS additionally exposes
input-buffer and pending-slot high waters. Logical output/input lengths and queue
entry counts exclude extra container capacity, retained trade vectors and kernel
socket buffers. A finite entry/buffer limit is not a small constant process-byte
budget. Lifetime accepted-ID storage can grow even when each paired workload
ends with an empty active book.

All scaling servers reported a startup high water of 1648 KiB. Their observed
process peaks and within-trial high-water differences were:

| Workload | Clients | Peak RSS min KiB | Peak RSS max KiB | Maximum high-water growth KiB |
| --- | ---: | ---: | ---: | ---: |
| ping | 1 | 1856 | 1872 | 224 |
| ping | 4 | 1936 | 1968 | 320 |
| ping | 8 | 2112 | 2128 | 480 |
| ping | 16 | 2448 | 2512 | 864 |
| matching | 1 | 2064 | 2096 | 448 |
| matching | 4 | 2576 | 2592 | 944 |
| matching | 8 | 3120 | 3168 | 1520 |
| matching | 16 | 4272 | 4464 | 2816 |
| cancellation | 1 | 2016 | 2032 | 384 |
| cancellation | 4 | 2320 | 2320 | 672 |
| cancellation | 8 | 2672 | 2720 | 1072 |
| cancellation | 16 | 3456 | 3504 | 1856 |

Across scaling runs, request and completion FIFO high waters reached 27 entries;
the largest logical per-connection output length was 1123 bytes. These remain
below the configured 1024-entry shared queues and 65536-byte output limit.
Overload servers used smaller queues and reported startup high water 1376 KiB,
peak ranges 1680–1712 KiB (four clients) and 2176–2272 KiB (sixteen clients),
with maximum high-water growth 336 and 896 KiB. These are observations for
bounded synthetic runs, not a claim of a constant memory footprint under
arbitrary order lifetimes.

## Preserve and compare core performance

The optional gateway leaves the matching library and original benchmark hot path
separate. Raw pre/post-upgrade core evidence is preserved as
[core-before-network.csv](measurements/core-before-network.csv) and
[core-after-network.csv](measurements/core-after-network.csv). Reproduce the same event count, seed,
trials, warmups, samples and reserve configuration before comparing them:

```sh
build/release/exchangecore_bench --events 1000000 --trials 7 --warmup 2 --samples 10000 --seed 60448 --reserve 0 --csv > benchmark-results/core-after-network.csv
python3 scripts/compare_benchmarks.py docs/measurements/core-before-network.csv benchmark-results/core-after-network.csv
```

Confirm matching activity, replay hashes and final-state hashes agree. Inspect
per-workload medians and variation, rather than attributing every timing change
to the gateway. Fresh-process comparisons still have allocator/cache, power and
scheduling differences. The historical original measurements remain preserved.

Both fresh core runs used one million events per workload, seven throughput
trials, two warmups, 10000 separate latency samples and reserve zero:

| Workload | Before median M events/s | After median M events/s | Median change | Before CV (%) | After CV (%) |
| --- | ---: | ---: | ---: | ---: | ---: |
| resting | 10.760 | 10.936 | +1.63% | 1.252 | 11.256 |
| crossing | 8.786 | 8.774 | -0.14% | 6.053 | 1.054 |
| cancellation | 15.937 | 15.998 | +0.39% | 1.106 | 2.000 |
| deep | 7.597 | 7.790 | +2.55% | 1.492 | 0.594 |
| touch | 12.158 | 12.184 | +0.21% | 0.790 | 0.839 |
| mixed | 14.703 | 14.972 | +1.83% | 0.273 | 0.883 |

Activity counters, full replay/state hashes and allocation measurements agree
between the runs. Median changes ranged from −0.14% to +2.55%; no material core
slowdown is apparent in these observations. The post-upgrade resting run had a
low outlier and CV 11.256%, so the positive changes are not evidence that adding
networking optimized the core. The original matching/benchmark hot paths were
preserved. Fresh mixed throughput was 14.703 M events/s before and 14.972 M/s
after; historical original-project measurements remain separately labeled in
[PERFORMANCE.md](PERFORMANCE.md).
