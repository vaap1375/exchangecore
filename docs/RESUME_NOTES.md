# Resume notes

## Verified engineering accomplishments

ExchangeCore combines a reusable C++20 price-time matching library with an
optional POSIX TCP order-entry gateway. Integer price ticks, stable map/list
iterators, indexed cancellation, lifetime order-ID tracking and allocation-failure
rollback preserve the original matching semantics. Networking is a separate
target; ordinary library calls acquire no gateway locks.

One `poll` I/O owner handles nonblocking sockets and per-connection buffers.
A dedicated matching thread owns the engine. Bounded request and completion
FIFOs carry owned values between them; global processing sequences, connection
tokens and correlated END-terminated responses define ordering and routing.
The gateway handles fragmented/coalesced frames, partial writes, disconnected
clients, queue saturation and graceful draining.

All test configurations and both benchmark suites were reverified in the
[2026-10-31 audit](VERIFICATION_2026-10-31.md) without production code changes.
Its matching TCP result was 39,970 requests/s median, with p50/p95/p99 RTT
781/1,138/1,310 µs, including Python client costs. Fresh mixed-core throughput
was 14.830M events/s; baseline/current benchmark executable bytes match and
timing variation is documented. The detailed evidence below is the preserved
2026-10-29 implementation checkpoint.

Verified evidence at that checkpoint:

- 48,000 deterministic events compared with an independent reference model;
  ordered executions and complete book state checked after each event.
- Seven allocation-failure scenarios covering 31 injected allocation sites,
  including rollback and retry with the same unconsumed order ID.
- 67 original named core/parser/reference cases, 594 benchmark generator
  combinations and 22 black-box CLI checks.
- 33 protocol tests, 12 bounded-queue tests and 13 TCP integration scenarios.
- All 12 CTest groups passed on macOS in Debug, Release, ASan/UBSan and TSan;
  no sanitizer findings were reported in those successful runs.
- Ten consecutive repetitions of the complete 13-scenario TCP integration
  suite passed, exercising concurrent load, backpressure and shutdown.
- All 12 CTest groups passed on Ubuntu 24.04 / GCC 13.3 aarch64 in Debug
  (14.91 s) and Release (5.12 s). All 12 also passed with Linux Clang 18
  ASan/UBSan/LeakSanitizer (11.38 s), without sanitizer findings.

The original million-event core benchmark measured **14.4–14.5 million synthetic
mixed input events/s** in two baseline processes on an Apple M1 Max, with seven
throughput trials per process. This is historical, in-process evidence from
before the TCP upgrade. See [PERFORMANCE.md](PERFORMANCE.md) for the original
timing boundaries, variation, hardware and raw data.

Fresh pre/post-upgrade core runs measured mixed medians of 14.703 and
**14.972 million events/s**. Across six workloads, median changes ranged from
−0.14% to +2.55%, with no material slowdown apparent; the resting post-run had
CV 11.256%, so positive differences are not evidence of a core optimization.
Activity, replay/state hashes and allocation measurements were preserved.

Separate TCP scaling runs completed 522,000 measured requests with zero
overloads, engine rejections or discarded completions. A representative
four-client matching workload measured **39,953.6 complete requests/s median**
over three Release trials on M1 Max, using Python clients and pipeline eight
per client. Its separate sampled RTT was p50 800.1 us, p95 1135.0 us and p99
1307.6 us. These rates include the client/GIL/socket/parser costs. A separate
queue-capacity-one experiment explicitly accounted for 44,029 overload replies
among 120,000 measured requests with no admitted completion loss. Full results
and limitations are in [NETWORK_PERFORMANCE.md](NETWORK_PERFORMANCE.md).

Technologies used: C++20, STL containers, RAII, POSIX sockets, `poll`,
`std::jthread`, mutexes, condition variables, atomics, CMake, Ninja, Apple Clang,
GCC, CTest, ASan, UBSan, LeakSanitizer, TSan, Python client/test/benchmark tooling, macOS `sample`
and local Linux Docker validation. GitHub Actions is configured; hosted CI has
not run during this upgrade.

## Three possible resume bullets

- Built a C++20 price-time matching engine with indexed cancellation and
  allocation-failure rollback; measured 14.97M synthetic mixed events/s median
  over seven Release trials on Apple M1 Max.
- Extended the engine with a POSIX TCP gateway using nonblocking `poll`, bounded
  queues and one matching writer; measured approximately 40k requests/s with
  four Python clients, pipeline eight per client, on Apple M1 Max.
- Differential-tested 48,000 seeded events and passed macOS ASan/UBSan/TSan and
  Linux Debug/Release/Clang sanitizer suites; repeated all 13 TCP integration
  scenarios ten times.

Use hardware, synthetic-workload and Python-client qualifiers when quoting
performance. Keep core events/s and TCP requests/s distinct. The approximately
40k rate is the matching workload's median, including complete client processing;
it is not an isolated maximum gateway-capacity claim.

## Five interview questions and answers

### 1. Who owns the order book, and when is an order acknowledged?

The matching writer exclusively owns the engine, including BEST queries. The
I/O thread owns socket descriptors, connection lifetime, framing and response
buffers. Owned requests/completions cross bounded mutex-protected FIFOs; their
locks are released before matching, serialization or socket I/O. This keeps
mutable book state on one thread and avoids sharing container iterators.

A successful request-FIFO push assigns its positive global processing sequence.
That is admission, rather than an order acknowledgment. The writer processes
FIFO order and returns an explicit engine OK or REJECT; only then can I/O send
the result. A queued engine rejection still has a positive sequence. Connection
tokens prevent a reused file descriptor from receiving an earlier peer's result.
Concurrent clients can have different admission orders between runs; the chosen
processing sequence defines the order actually executed.

### 2. How do partial I/O and broken connections affect framing?

TCP supplies a byte stream. The I/O owner retains incomplete input, extracts
bounded LF-delimited EC1 requests and handles multiple frames in one read.
Responses contain correlated headers, optional TRADE lines and END. Serialization
can stream a large response through the bounded output buffer. A successful
`send` advances only its returned byte count; EAGAIN/EWOULDBLOCK returns to
`poll`, and interrupted calls are handled explicitly.

Linux sends use `MSG_NOSIGNAL`; macOS configures `SO_NOSIGPIPE`, so a broken pipe
does not terminate the process. A reset during accepted-socket setup is handled
as a local connection failure. Admitted work remains owned by the writer after
disconnection; replies for a missing connection token are discarded rather than
misrouted. These rules were tested with fragmentation, forced seven-byte write chunks,
half-close, reset/reconnect and cross-client routing scenarios.

### 3. What prevents overload or shutdown from deadlocking the gateway?

Request/completion FIFO capacities, per-client pending slots and input/output
bounds limit admitted buffering. A full request FIFO returns OVERLOADED with
sequence zero when a response slot is available. Client reads pause at their
pending/output limits. The writer can wait for completion capacity while the
I/O owner continues draining completions without waiting for one slow socket
to become writable. Socket writes never hold a queue mutex.

Normal shutdown stops new admission, closes request production and drains
already admitted work. The response grace period starts after the writer
finishes; slow peers cannot extend that configured period. Exceptional I/O
cleanup closes both queues before joining, which releases a writer blocked on
completion publication. Finite entry limits are not a small fixed byte budget:
retained trade vectors and container capacities still consume memory. Processing
all admitted requests also does not guarantee remote receipt after disconnection
or grace expiry.

### 4. What can a client safely retry after overload or a lost connection?

Request IDs strictly increase within each connection. A syntactically valid
OVERLOADED request consumes its request ID but never enters matching; retrying
requires a higher request ID. Order IDs have a different, shared lifetime: an
accepted new order's ID remains consumed after fill, cancellation or reconnect.
Submitting that same accepted order ID again receives DUPLICATE_ID and cannot
execute it a second time. A rejected new order leaves its order ID available.

If a connection fails after admission, the client may not know whether matching
completed. Reusing an order ID prevents a second accepted new order but cannot
reconstruct the lost execution response. A repeated cancellation can return
UNKNOWN_ORDER after the first succeeded. There is no durable request journal,
response replay cache or exactly-once response-delivery guarantee. The benchmark
therefore never retries measured requests, including overloads; it reports their
outcomes separately.

### 5. What do the performance measurements establish?

The core harness measures engine processing with result destruction and a small
digest, excluding workload generation, parsing, printing and invariant checks.
Latency and allocation instrumentation use separate replays. Standard node
containers simplify ownership and cancellation but pay allocation/locality costs;
profiling and a controlled hash-reservation experiment made those costs visible.
Reservation remains configurable because its memory and throughput effects vary
by workload.

The network harness measures complete TCP requests, including Python client
threads, the GIL, socket framing, scheduling and response validation. It uses
bounded pipelines, repeated throughput trials and a separate sampled RTT replay
from before send through receipt/decoding of END. Completed-request throughput
includes overload replies; processed-request throughput counts positive-sequence
responses, including engine rejections. Neither metric isolates the gateway's
maximum server-side capacity. Queue occupancy and server process RSS are separate
observations; peak-minus-startup RSS is high-water growth, not live heap usage.

## How this complements TaskFlow

Position TaskFlow around the Java and distributed application engineering the
owner has already demonstrated. Position ExchangeCore around explicit memory
ownership, iterator lifetime, integer arithmetic, POSIX byte-stream handling,
in-process thread synchronization and reproducible systems measurements. The
single matching writer is a deliberate ordering choice; concurrency belongs
around that owner. This adds systems depth without duplicating TaskFlow's Java
application story or attributing unverified features to that project.

## Limits and current readiness

The implemented gateway, measured performance and completed platform checks
support inclusion as a local systems portfolio project. Hosted CI remains
unverified. A Linux GCC ASan reference configuration exceeded its 180-second
timeout without a sanitizer diagnostic; its exact cause was not established,
and that run is not a pass. Linux Clang 18 ASan/UBSan/LeakSanitizer subsequently
passed all 12 CTest groups. macOS sanitizer results above also refer to actual
successful runs, rather than inferred portability.

The simulated gateway has no authentication, TLS,
account isolation, durable recovery or production traffic validation. Keep its
default loopback binding. Finite resource limits and tested failure handling do
not make it a production exchange. Publication remains the owner's decision;
this upgrade has not committed, pushed or deployed anything.
