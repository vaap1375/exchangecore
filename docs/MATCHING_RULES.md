# Matching and input contract

Prices are positive `int64_t` integer ticks. Quantity and order ID are positive
`uint64_t` integers. In the synthetic ES display configuration one tick is 0.25
index points: 24001 ticks displays as 6000.25. The matching key is never floating
point. The example configuration models matching mechanics, not a real venue's
complete protocol or rules.

1. A buy takes the lowest ask first; a sell takes the highest bid first.
2. At one price, makers execute in arrival sequence order (FIFO).
3. Every trade uses the resting maker's price and a positive quantity equal to
   the smaller of the maker's remaining quantity and the incoming remainder.
4. A limit order stops at its limit. Any remainder rests with its original
   incoming sequence. A market order consumes available liquidity and discards
   the unfilled remainder; it never rests.
5. Filled and canceled orders disappear from the active index. Empty price
   levels are removed. A cancellation targets only an active ID.

Every **accepted new order**, including an unmatched market, permanently consumes
its ID in that engine instance. A rejected new order does not consume its ID.
Canceling an unknown, filled or already canceled ID rejects with `UnknownOrder`.
Cancel never releases a previously accepted ID. There is no session reset API;
construct a new engine for a new ID lifetime.

`Result.filled + Result.remaining` equals requested quantity for a new order.
On rejection filled is zero and remaining is the requested quantity. A limit's
remaining quantity rests; a market's remaining quantity expires. A successful
cancel has `canceled` equal to removed quantity, filled/remaining zero, no trades.
Rejections never produce trades. Results and snapshots have value equality and
contain no wall-clock data.

Validation precedes mutation: ID, side enum, order-type enum, quantity, price,
duplicate ID, lifetime capacity, aggregate quantity, then sequence exhaustion.
Market price must be zero in the C++ API. Cancel reads only its ID. Unknown event
enums reject. A zero ID cancellation rejects as an invalid ID.

The stored aggregate quantity is `uint64_t`. New limits conservatively reject
when `requested quantity + current total resting quantity` would overflow, even
if execution could reduce the eventual total. This rule keeps arithmetic exact
and validation independent of liquidity sweeps. Market orders do not add resting
quantity and are exempt from this guard. A single maximum-quantity order and a
maximum positive price are valid when other guards permit them. Prices are only
compared and formatted; the engine does not multiply price by quantity.

The sequence counter rejects before wrapping. Capacity and sequence exhaustion
are explicit rejections. The library does not silently evict accepted IDs.

Allocation failures propagate as `std::bad_alloc`. Completed semantic state is
preserved if staging fails; internal hash bucket capacity may have changed. The
engine can be queried and the same operation retried after the resource issue
is addressed. It does not promise availability under process-level memory
exhaustion, and storing the returned `Result` is the caller's responsibility.

## CSV replay

The first nonblank, noncomment line must be this case-sensitive header:

```csv
type,id,side,quantity,price
LIMIT,1,SELL,10,24001
MARKET,2,BUY,4,
CANCEL,1,,,
```

Exactly five unquoted fields are required. Fields accept surrounding spaces,
tabs and CRLF endings. Blank lines and lines starting with `#` after trimming
are ignored. A final line needs no newline. An empty or comment-only file is a
valid zero-event stream. Quotes, scientific notation, plus signs, decimal prices,
negative quantities and out-of-range integers reject. LIMIT requires BUY/SELL,
quantity and tick price; MARKET requires an empty price; CANCEL requires empty
side, quantity and price. Zero IDs and quantities reject.

Input is bounded to 4096 bytes per line, one million events and 100 syntax
diagnostics. Overlong lines are discarded with bounded buffering. Replay parses
before applying events, so syntax errors cause no executions.
Semantic rejections are reported at the original file line and processing
continues with subsequent events. `validate` checks both syntax and semantics
using a fresh engine. CLI exit codes: 0 success, 1 file/runtime failure, 2 invalid
arguments, syntax errors or semantic rejections.

`replay --quiet` suppresses per-event output, retaining summary and final depth
(at most ten displayed levels per side). The library snapshot has no such
display limit. Identical input produces identical replay output.
