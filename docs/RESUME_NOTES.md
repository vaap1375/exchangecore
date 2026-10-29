# Resume notes

## Verified accomplishments

- Built a reusable C++20 library and CLI for deterministic price-time limit and
  market matching, multi-level execution, indexed cancellation and depth queries.
- Designed map/list price levels, iterator-based active lookup and lifetime ID
  tracking with precise expected/amortized complexity and explicit overflow guards.
- Implemented allocation-failure rollback by preparing results and staging state
  before committing fills. Seven fault scenarios verified 31 allocation sites.
- Compared 48,000 seeded events against an independent vector/scan reference
  model, checking ordered executions and complete state after every event.
- Passed all eight CTest groups in Debug, Release and ASan/UBSan on Apple Clang;
  67 named core/parser/reference cases, 594 benchmark generator combinations and
  22 black-box CLI checks. Clang static analysis reported no findings.
- Measured six one-million-event workloads with seven trials and separate latency
  and allocation instrumentation. Mixed baseline repeated at 14.4–14.5M events/s
  on an Apple M1 Max. Read `PERFORMANCE.md` before quoting numbers.

Technologies actually used: C++20, STL ordered/hash containers, RAII, CMake,
Ninja, Clang, ASan, UBSan, CTest, dependency-free C++ testing, Python CLI test
automation, macOS sample, CSV replay and GitHub Actions configuration. Hosted CI
has not run; no remote or commits were created.

## Three possible bullets

- Built a deterministic C++20 order matching engine with price-time priority,
  multi-level fills and expected O(1) indexed cancellation using stable
  map/list iterators and RAII ownership.
- Validated ordered trades and full book state against an independent reference
  model over 48,000 seeded events; passed ASan/UBSan and fault-injected 31
  allocation points to verify rollback and safe same-ID retry.
- Benchmarked six million-event workloads at 7.8–16.2M events/s median on Apple
  M1 Max; profiled container/allocation costs and evaluated configurable hash
  reservation with repeated deep-book gains and explicit memory tradeoffs.

If space is tight, use the first two and a shorter performance clause. Label
workloads synthetic and keep hardware context. Do not claim real-world exchange
latency, production deployment, or a general optimization percentage.

## Engineering tradeoffs

- Single-threaded ownership simplifies deterministic arrival order and removes
  synchronization, but the caller must serialize queries and input.
- Standard map/list nodes provide stable handles and clear ownership; node
  allocation and locality limit performance versus specialized intrusive layouts.
- Preparing trades before commit costs a second maker traversal while making
  allocation failure recoverable without copying the entire book.
- Lifetime IDs use bounded but growing memory. Conservative aggregate overflow
  validation can reject a crossing limit at extreme integer boundaries.
- Reservation helps some large workloads, consumes upfront buckets, and can
  increase gross requested bytes for cancellation/mixed traffic. Small-book
  speed gains did not reproduce consistently.

## Interview discussion topics

1. Walk through partial multi-level fills and canceling the middle of a FIFO:
   which iterators stay valid, what is erased, and why the next maker is correct?
2. Explain the independent model, quantity conservation, seed reproduction,
   invalid-input atomicity and the allocation preparation/commit boundary.
3. Explain benchmark timing boundaries, percentile quantization, reverse-order
   experiments, RSS versus allocation requests, and why a noisy gain was not
   made the default.

## Limits and readiness

Ready to discuss and include as a verified local systems project. Ready for a
GitHub review once the owner chooses to publish; hosted CI and Linux sanitizer
runtime remain unverified. It is not a production exchange. No networking,
durable journal, concurrency, modify/replace, brokerage integration or real trades
are implemented. No unsupported features or platform results belong in a resume.
