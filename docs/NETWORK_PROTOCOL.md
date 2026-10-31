# EC1 TCP protocol

The optional `exchangecore_gateway` executable serves one shared, simulated order
book over IPv4 TCP. It binds to `127.0.0.1:9000` by default. Start it with:

```sh
build/release/exchangecore_gateway --host 127.0.0.1 --port 9000
```

Port zero selects an ephemeral port. Startup prints `LISTEN host actual_port` to
the server's standard output; this line is not sent over client connections.
The original matching library and CLI continue to work without this server.

## Framing and numbers

Each request is one ASCII line terminated by LF. CRLF is accepted: one terminal
CR is removed before parsing. The default maximum is **4096 bytes before LF**,
including that optional CR. Thus a 4096-byte frame plus LF is valid; 4097 bytes
before LF is oversized. `--max-input` can lower the limit.

Space and tab separate fields; repeated separators and surrounding spaces/tabs
are allowed. Fields and command names are case sensitive. Embedded CR, NUL,
other control bytes and non-ASCII bytes reject. There are no quoted strings,
comments, optional fields or floating-point numbers. TCP packet boundaries have
no protocol meaning: one read may contain part of a line or several lines.

Request IDs, order IDs and quantities are positive decimal `uint64_t` integers
through `18446744073709551615`. Prices are positive decimal `int64_t` integer
ticks through `9223372036854775807`. Signs, decimal points, exponent notation,
trailing numeric junk and overflow reject. Leading zeroes are allowed. One
synthetic ES tick represents 0.25 points: `24001` ticks represents `6000.25`.

## Requests

`rid` is the client-generated correlation ID; it is separate from the order ID.

| Command | Exact fields | Example |
|---|---|---|
| Limit | `EC1 rid LIMIT order_id BUY\|SELL quantity ticks` | `EC1 1 LIMIT 101 SELL 10 24001` |
| Market | `EC1 rid MARKET order_id BUY\|SELL quantity` | `EC1 2 MARKET 102 BUY 4` |
| Cancel | `EC1 rid CANCEL order_id` | `EC1 4 CANCEL 101` |
| Best prices | `EC1 rid BEST` | `EC1 3 BEST` |
| Health | `EC1 rid PING` | `EC1 5 PING` |

Extra fields reject, including a price field on MARKET. BEST and PING pass
through the same processing queue as order events. BEST observes the book at
that request's position in the matching sequence.

## Responses

Every response line begins with `EC1 rid sequence`. A complete response contains
one header, followed by its ordered TRADE records when applicable, and a matching
END line. Records from different requests are not interleaved on one connection.
Each generated line is shorter than 256 bytes including LF; a complete response
can contain many lines and is streamed.

The reference Python client independently caps a complete reply at 1 MiB by
default. A library caller can set `FrameClient(max_reply_bytes=...)` when testing
larger sweeps; a reply exceeding the client limit raises a diagnostic and closes
that client. This client bound does not change whether the server processes an
admitted request.

```text
EC1 rid sequence OK order_id filled remaining canceled trade_count
EC1 rid sequence REJECT reason order_id remaining
EC1 rid sequence TRADE maker_id taker_id ticks quantity
EC1 rid sequence BEST bid_or_NONE ask_or_NONE active_orders total_resting_quantity
EC1 rid sequence PONG
EC1 rid sequence ERROR code
EC1 rid sequence END
```

OK, REJECT, BEST, PONG and ERROR are alternative headers. OK reports the matching
engine's result, rather than merely receipt by the socket or admission to a
queue. New orders have `filled + remaining = requested quantity`. Limit
remainders rest; market remainders expire. A successful CANCEL reports the
removed quantity in `canceled`, with filled/remaining/trade_count all zero.
REJECT has no trades and leaves matching state unchanged. Missing best prices
use the literal `NONE`; all displayed prices remain integer ticks.

For example, send these five requests in order on a fresh server:

```text
EC1 1 LIMIT 101 SELL 10 24001
EC1 2 MARKET 102 BUY 4
EC1 3 BEST
EC1 4 CANCEL 101
EC1 5 PING
```

Their complete responses are:

```text
EC1 1 1 OK 101 0 10 0 0
EC1 1 1 END
EC1 2 2 OK 102 4 0 0 1
EC1 2 2 TRADE 101 102 24001 4
EC1 2 2 END
EC1 3 3 BEST NONE 24001 1 6
EC1 3 3 END
EC1 4 4 OK 101 0 0 6 0
EC1 4 4 END
EC1 5 5 PONG
EC1 5 5 END
```

Replacing the second request with `EC1 2 LIMIT 102 BUY 4 24001` produces the same
fill and trade. Executions go to the submitting request's connection. The server
does not broadcast an unsolicited execution notification to the resting maker.

## Processing sequence and request IDs

The I/O owner assigns a positive, globally increasing processing sequence only
after admission to the bounded request FIFO succeeds. All five commands consume
one sequence. The matching writer processes this FIFO in order. An engine-level
rejection still has a positive sequence because processing took place.
This gateway sequence is separate from the core's arrival counter, which advances
only for accepted new orders.

Errors decided before queue admission have sequence **zero**. For example:

```text
EC1 8 0 ERROR OVERLOADED
EC1 8 0 END
```

An exceptional failure while the writer attempts a queued request produces
`ERROR INTERNAL` with that request's positive sequence. It does not acknowledge
a successful execution.

Request IDs must strictly increase on each connection. A syntactically valid
request with an ID greater than the connection's previous ID consumes that ID
before queue admission, including a request rejected with OVERLOADED. Retry an
overloaded request using a higher request ID. A lower or repeated valid ID
produces `ERROR DUPLICATE_REQUEST` and is not queued. Syntax errors do not advance
the connection's ID. A syntax-error response preserves a valid second token as
its request ID when available; otherwise the correlation ID is zero. Oversized
frames use zero. After using the maximum request ID, reconnect to start a new
request-ID namespace.

Responses follow frame order on each connection, including sequence-zero errors.
Independent clients may use the same request IDs. Their actual admission order
depends on socket readiness and scheduling and can differ across runs. The
processing sequence describes the chosen order; replaying an identical ordered
sequence retains the core engine's deterministic behavior. No durable network
request journal or reconnect replay service is implemented.

Order IDs belong to the shared engine, across every client connection. Once a
new order is accepted, its ID cannot be reused during that server lifetime,
including after fill, cancellation or reconnect. Repeating the same accepted
order ID with a fresh request ID produces `REJECT DUPLICATE_ID`. A rejected new
order does not consume its order ID. The server does not cache/replay previous
responses or promise exactly-once response delivery. If a connection fails after
admission, processing can still occur even when the client receives no reply.

## Errors and connection behavior

Syntax codes are `INVALID_CHARACTER`, `INVALID_VERSION`, `INVALID_REQUEST_ID`,
`UNKNOWN_COMMAND`, `FIELD_COUNT`, `INVALID_ORDER_ID`, `INVALID_SIDE`,
`INVALID_QUANTITY` and `INVALID_PRICE`. In-bounds malformed lines receive ERROR
and END; the connection remains usable. Error text consists of fixed tokens,
never arbitrary request text.

Gateway codes also include `DUPLICATE_REQUEST`, `OVERLOADED`,
`SEQUENCE_EXHAUSTED`, `LINE_TOO_LARGE`, `INCOMPLETE_FRAME` and `INTERNAL`.
OVERLOADED means no queue admission occurred and no matching operation was
attempted. A sequence-exhaustion error also occurs before admission. Engine
rejection tokens include `DUPLICATE_ID`, `UNKNOWN_ORDER`, `CAPACITY_EXCEEDED`,
`QUANTITY_OVERFLOW` and `SEQUENCE_EXHAUSTED`; their matching semantics are in
[MATCHING_RULES.md](MATCHING_RULES.md).

An oversized frame stops further reads on that connection. The server attempts
to send earlier responses, followed by `ERROR LINE_TOO_LARGE` and END, then
closes it. A normal write half-close permits queued responses to finish. If FIN
arrives with a nonempty final line lacking LF, that line is not admitted; the
server attempts `ERROR INCOMPLETE_FRAME` and END before closing. An abrupt reset
or failed write can prevent any further response from reaching that peer.
Already admitted requests continue processing after disconnection.

When a connection has reached its pending-response or output-buffer limit,
reading pauses until capacity is available. A full shared request FIFO returns
OVERLOADED when a response slot is available. Excess connections beyond the
configured client cap are closed without a protocol response. SIGINT/SIGTERM
stop new admission, drain admitted work and allow a configurable response grace
period; see [CONCURRENCY.md](CONCURRENCY.md).

This endpoint has no authentication, TLS, per-client order ownership, durable
recovery or public-network hardening. Any connected client can cancel an active
order by its global ID. Keep the default loopback binding for the local demo;
this simulated gateway is not a secure public trading endpoint.
