# Data structures, complexity and ownership

Let N be active orders, L be price levels on a side, K be makers filled or
partially filled, D be removed price levels, and U be all accepted IDs.

| Operation | Complexity and conditions |
|---|---|
| Best bid / ask | O(1), `rbegin()` / `begin()` on ordered maps |
| Find active ID | Expected O(1), hash worst case O(N) |
| Noncrossing limit | O(log L) level lookup/insertion + expected amortized O(1) indexes |
| Price level insertion | O(log L) ordered map insertion |
| Price level deletion by stored iterator | Amortized O(1) for `std::map::erase(iterator)` |
| Matching | O(K + D), plus O(log L) if a limit remainder rests; hash erasures expected O(1) each |
| Cancel | Expected O(1) ID lookup, list erase and iterator-based level erase (amortized) |
| Depth of H levels | O(min(H,L)), cached aggregate per level |
| Full order snapshot | O(N), plus allocation of copied values |
| Full invariant check | O(N + L), expected with hash containers |

Hash-table insertion is amortized expected constant time; rehash can be linear.
Hash lookup and deletion have linear worst cases. Complexity statements for
matching assume average hash behavior. Preparation and commit each visit the
matched makers, preserving asymptotic O(K). Allocation and result-vector growth
are real constant factors measured in benchmarks.

Each side uses `std::map<Price, PriceLevel>`. A level owns
`std::list<RestingOrder>` plus aggregate quantity. Nodes preserve FIFO and stable
iterators on unrelated inserts/erases. A hash map indexes ID to side, price-level
iterator and list iterator. Cancellation never searches the book. A separate
hash set holds all accepted IDs to prevent reuse after retirement.

Maps/lists/hash containers own all memory through RAII. The location index
contains nonowning iterators. Removing an order removes its location and list
node; a price-level iterator is erased only when its list is empty. No surviving
location refers to that erased level. Rehashing the location table does not move
the map/list nodes it references. Queries return copies, not borrowed handles.

Space is O(N + L + U + K) including the current result's trades. Unlike the
active-order count, U grows across fills and cancellations, until the configured
lifetime capacity rejects further new orders. Hash reservation is configurable;
it trades upfront bucket memory for fewer rehashes. The engine uses standard
allocation rather than a custom memory pool without demonstrated need.

Invariant checks verify positive resting quantities/prices, correct side/level,
FIFO sequences, valid indexed IDs and iterators, unique orders, level aggregates,
the total aggregate, accepted-ID membership, and an uncrossed book. Tests call
them explicitly. The Debug/sanitizer presets also enable checks after events;
the Release preset excludes automatic expensive checks.
