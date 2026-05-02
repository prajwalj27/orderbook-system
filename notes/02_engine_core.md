# Engine Core — C++17 Matching Engine

## What is an Order Book? (CS Perspective)

An order book is a server that maintains two sorted lists and continuously looks for deals between buyers and sellers of a stock.

- **Bids** — all buy orders, sorted by price **descending** (highest willingness to pay first)
- **Asks** — all sell orders, sorted by price **ascending** (lowest price a seller accepts first)

```
ASKS (sellers)
──────────────
$152.00  | 200 shares
$151.00  | 100 shares   ← best ask (cheapest seller)
         ↑ SPREAD
$150.00  | 100 shares   ← best bid (richest buyer)
$148.00  | 500 shares
──────────────
BIDS (buyers)
```

A **match** (trade) happens when a buyer's price >= a seller's price. The engine runs continuously watching for this crossing condition.

**Price-time priority:** among orders at the same price, whoever arrived first gets matched first.

---

## Order Types

| Type | Behaviour | No match found | Partial match |
|---|---|---|---|
| LIMIT | Trade only at specified price or better | Rests in book | Remainder rests |
| MARKET | Trade immediately at any price | Cancelled | Remainder cancelled |
| IOC | Trade now or never at limit price, no resting | Cancelled | Remainder cancelled |

**LIMIT** — safe and patient. "I'll only buy at $150.00 or cheaper."
**MARKET** — aggressive and price-blind. "Fill me now at whatever price exists."
**IOC** — aggressive but controlled. "Fill what you can right now, cancel the rest. Never leave a footprint in the book."

---

## File-by-File Breakdown

---

### `include/Order.h`

The fundamental data unit. Every other component depends on this file.

```cpp
#pragma once
#include <string>
#include <cstdint>

enum class Side        { BUY, SELL };
enum class OrderType   { LIMIT, MARKET, IOC };
enum class OrderStatus { NEW, PARTIAL, FILLED, CANCELLED };

struct Order {
    std::string  orderId;    // FIX tag 11 — client-assigned unique ID
    Side         side;       // FIX tag 54 — BUY or SELL
    OrderType    type;       // FIX tag 40 — LIMIT, MARKET, or IOC
    double       price;      // FIX tag 44 — omitted for MARKET orders
    int          quantity;   // FIX tag 38 — total shares requested
    int          filledQty;  // shares filled so far, starts at 0
    OrderStatus  status;     // current lifecycle state
    int64_t      timestamp;  // nanoseconds since epoch
};
```

**Key concepts:**

**`#pragma once`** — header guard. Tells the compiler to only include this file once per compilation unit even if multiple files include it. Without it you get "redefinition" errors.

**`enum class` vs plain `enum`** — scoped enums. Names are accessed as `Side::BUY` not just `BUY`. Prevents name clashes and implicit conversion to int.
```cpp
enum Side { BUY, SELL };       // plain — BUY leaks into scope, clashes possible
enum class Side { BUY, SELL }; // scoped — must use Side::BUY, safe
```

**`struct` not `class`** — `struct` members are public by default. Order is pure data with no private internals or methods, so `struct` is the right choice.

**`int64_t` for timestamp** — plain `long` is platform-dependent (32-bit on some systems). `int64_t` is always exactly 64 bits, required for nanosecond timestamps that exceed 32-bit range. Needs `<cstdint>`.

**`double price`** — in real production engines this is a fixed-point integer (price in cents) to avoid floating-point precision issues. We use `double` here to keep matching logic readable. A known tradeoff worth mentioning in interviews.

---

### `include/OrderBook.h` + `src/OrderBook.cpp`

Holds the two sides of the book and exposes an interface for the matching engine.

```cpp
class OrderBook {
public:
    void addOrder(const Order& order);
    bool cancelOrder(const std::string& orderId);

    std::map<double, std::deque<Order>>& getBids();
    std::map<double, std::deque<Order>>& getAsks();

    bool hasBids() const;
    bool hasAsks() const;

private:
    std::map<double, std::deque<Order>> bids;
    std::map<double, std::deque<Order>> asks;
    std::unordered_map<std::string, Order*> orderIndex;
};
```

**Data structure choices (all locked, all deliberate):**

**`std::map<double, std::deque<Order>>`** for bid/ask sides:
- `std::map` keeps price levels sorted at all times. Best bid = `rbegin()` (last element = highest), best ask = `begin()` (first element = lowest). Both are O(1).
- `std::unordered_map` would require O(n) scan to find the best price — defeats the purpose.
- `std::deque<Order>` per price level — orders at the same price are served FIFO (time priority). `pop_front()` is O(1). `std::vector::pop_front` shifts all elements = O(n) per fill.

**`std::unordered_map<std::string, Order*>`** for order lookup:
- Used exclusively for cancel operations. Find any order by ID in O(1).
- Stores a raw pointer to the `Order` inside the deque.
- Sorting is irrelevant for cancel lookup, so no `std::map` overhead.

**`getBids()`/`getAsks()` return non-const references** — the matching engine needs to directly manipulate these maps (pop filled orders, erase empty price levels). A reference gives direct access.

---

**`addOrder` implementation:**

```cpp
void OrderBook::addOrder(const Order& order) {
    if (order.side == Side::BUY) {
        bids[order.price].push_back(order);
        orderIndex[order.orderId] = &bids[order.price].back();
    } else {
        asks[order.price].push_back(order);
        orderIndex[order.orderId] = &asks[order.price].back();
    }
}
```

Two C++ guarantees make this safe:
1. `std::map::operator[]` creates an empty deque automatically if the price level doesn't exist yet.
2. `std::deque::push_back` does **not** invalidate pointers to existing elements (unlike `std::vector`). So as more orders arrive at the same price level, all existing `Order*` pointers in `orderIndex` remain valid.

---

**`cancelOrder` and the tombstone pattern:**

```cpp
bool OrderBook::cancelOrder(const std::string& orderId) {
    auto it = orderIndex.find(orderId);
    if (it == orderIndex.end()) return false;

    it->second->status = OrderStatus::CANCELLED;
    orderIndex.erase(it);
    return true;
}
```

**Why not physically remove the order from the deque?**
Erasing from the middle of a `std::deque` invalidates **all** references and pointers to all other elements. That would corrupt every `Order*` in `orderIndex`. Instead we use the tombstone pattern:
- Mark the order `CANCELLED` in-place
- Remove it from `orderIndex` (so nothing can find it again)
- The order stays in the deque until the matching engine pops it off the front

**Unpacking `it->second->status`:**
- `it` — an iterator pointing to one entry in `orderIndex` (a `std::pair<string, Order*>`)
- `it->first` — the key (order ID string)
- `it->second` — the value (the `Order*` pointer)
- `it->second->status` — dereference the pointer and access the status field

`->` means "dereference this pointer and access a member." It's shorthand for `(*ptr).member`.

**Why erase the iterator after marking cancelled?**
Two reasons:
1. Prevents double-cancel — the order shouldn't be findable for future cancel requests.
2. Safety — the `Order*` pointer will eventually become dangling when the matching engine calls `pop_front()` and destroys the object. Removing it from `orderIndex` now guarantees nobody follows a dangling pointer.

**`auto` keyword** — type inference. The compiler deduces the type from the right-hand side.
```cpp
// Without auto — verbose and hard to read:
std::unordered_map<std::string, Order*>::iterator it = orderIndex.find(orderId);
// With auto — identical result, compiler works it out:
auto it = orderIndex.find(orderId);
```

---

### `include/MatchingEngine.h` + `src/MatchingEngine.cpp`

The core logic. Takes an incoming order, finds counterparties, and returns fills.

```cpp
struct Trade {
    std::string buyOrderId;
    std::string sellOrderId;
    double      price;
    int         quantity;
    int64_t     timestamp;
};

class MatchingEngine {
public:
    explicit MatchingEngine(OrderBook& book);
    std::vector<Trade> processOrder(Order& order);
private:
    OrderBook& book;
    std::vector<Trade> matchBuy(Order& order);
    std::vector<Trade> matchSell(Order& order);
};
```

**`explicit` keyword** — prevents the compiler from using this constructor for implicit conversions. Without it, the compiler might silently construct a `MatchingEngine` where you didn't intend one.

**Takes `OrderBook& book` by reference** — the engine doesn't own the book, it operates on it. A reference member must be bound in the initialiser list, not assigned in the constructor body.

**`processOrder` returns `std::vector<Trade>`** — the engine collects all fills and hands them to the caller (`main.cpp`), which then sends them over TCP to the consumer. Engine stays focused on matching only.

**`matchBuy` and `matchSell` are private** — implementation detail. Outside code only calls `processOrder`.

---

**The matching loop (matchBuy):**

```cpp
while (order.filledQty < order.quantity) {
    if (asks.empty()) break;

    auto bestAskIt = asks.begin();              // lowest ask = best for buyer
    double bestAskPrice = bestAskIt->first;

    // price crossing check — MARKET orders skip this
    if (order.type != OrderType::MARKET && bestAskPrice > order.price) break;

    auto& levelDeque = bestAskIt->second;

    // skip tombstoned cancelled orders at front of deque
    while (!levelDeque.empty() && levelDeque.front().status == OrderStatus::CANCELLED) {
        levelDeque.pop_front();
    }
    if (levelDeque.empty()) { asks.erase(bestAskIt); continue; }

    Order& resting = levelDeque.front();
    std::string sellOrderId = resting.orderId;  // save BEFORE potential pop_front

    int fillQty = std::min(order.quantity  - order.filledQty,
                           resting.quantity - resting.filledQty);

    order.filledQty   += fillQty;
    resting.filledQty += fillQty;

    order.status   = (order.filledQty   == order.quantity)   ? OrderStatus::FILLED : OrderStatus::PARTIAL;
    resting.status = (resting.filledQty == resting.quantity) ? OrderStatus::FILLED : OrderStatus::PARTIAL;

    if (resting.status == OrderStatus::FILLED) {
        levelDeque.pop_front();
        if (levelDeque.empty()) asks.erase(bestAskIt);
    }

    trades.push_back({ order.orderId, sellOrderId, bestAskPrice, fillQty, now_ns() });
}
```

**Why save `sellOrderId` before `pop_front`:**
```cpp
std::string sellOrderId = resting.orderId;  // copied out to a local string
levelDeque.pop_front();                     // destroys the resting Order object
// resting.orderId would now be reading freed memory — use sellOrderId instead
```

**Fill quantity = `std::min` of both sides' remainders:**
You can only fill as much as both parties have available.

**Price crossing condition:**
- BUY order crosses ask when: `bestAskPrice <= order.price`
- SELL order crosses bid when: `bestBidPrice >= order.price`
- MARKET orders skip the check entirely — match at any price

**matchSell uses `std::prev(bids.end())` instead of `rbegin()`:**
```cpp
auto bestBidIt = std::prev(bids.end());  // highest bid = last element of ascending map
```
`bids.rbegin()` gives a reverse iterator. `bids.erase()` requires a forward iterator. `std::prev(end())` gives the last element as a forward iterator — same element, compatible with erase.

**Remainder handling after the loop:**
```cpp
if (order.filledQty < order.quantity) {
    if (order.type == OrderType::LIMIT) {
        book.addOrder(order);      // rest in book at limit price
    } else {
        order.status = OrderStatus::CANCELLED;  // MARKET or IOC — never rest
    }
}
```

**Member initialiser list:**
```cpp
MatchingEngine::MatchingEngine(OrderBook& book) : book(book) {}
//                                              ^^^^^^^^^^^
//                                              initialiser list
```
Reference members must be initialised here, not in the constructor body. References in C++ must be bound at construction and can never be reseated.

---

### `include/Parser.h` + `src/Parser.cpp`

Converts a raw FIX string into an `Order` or a cancel request.

```cpp
enum class MessageType { NEW_ORDER, CANCEL, UNKNOWN };

struct ParsedMessage {
    MessageType type;
    Order       order;      // valid when type == NEW_ORDER
    std::string cancelId;   // valid when type == CANCEL
};

class Parser {
public:
    static ParsedMessage parse(const std::string& raw);
};
```

**`parse` is `static`** — no state between calls. Pure transformation: string in, `ParsedMessage` out. Call as `Parser::parse(line)` with no object.

**`extractTags` helper — how it splits a FIX string:**
```cpp
// Input: "35=D|11=ORD001|54=1|44=150.10|38=500"
std::stringstream ss(raw);          // wrap in stream
std::getline(ss, token, '|');       // read up to next '|'
// Each token: "35=D", "11=ORD001", etc.

token.find('=')                     // find position of '='
token.substr(0, eq)                 // key:   "11"
token.substr(eq + 1)                // value: "ORD001"
```

Result: `{ "35"→"D", "11"→"ORD001", "54"→"1", "44"→"150.10", "38"→"500" }`

**Order type inference (from design doc spec):**
```cpp
bool hasPrice = tags.count("44") > 0;

if (tags.count("40") > 0) {
    // tag 40 explicit: 1=MARKET, 2=LIMIT, 3=IOC
} else {
    // tag 40 absent: infer from presence of price tag
    msg.order.type = hasPrice ? OrderType::LIMIT : OrderType::MARKET;
}
```

**`std::stod` / `std::stoi`** — convert strings to numbers.
- `std::stod("150.10")` → `150.10` (double)
- `std::stoi("500")` → `500` (int)
- Both throw `std::invalid_argument` on bad input. Production code uses `std::from_chars` (no exceptions, faster).

---

### `include/EventLog.h` + `src/EventLog.cpp`

In-memory audit trail. Records every significant order event and formats FIX execution reports.

```cpp
struct Event {
    OrderStatus type;
    std::string orderId;
    int         cumQty;    // FIX tag 14 — total filled so far
    int         leavesQty; // FIX tag 151 — quantity still open
    double      lastPx;    // FIX tag 31 — fill price (0 for ACK/CANCEL)
    int64_t     timestamp;
};
```

**Why reuse `OrderStatus` as event type** — the two concepts map 1:1. A separate `EventType` enum would duplicate the same four values.

**`lastPx = 0.0` default** — ACK and CANCEL events have no fill price. Default means callers only pass `lastPx` when recording a fill.

**Execution report format (FIX protocol):**
```
ACK:     39=0|11=ORD001|14=0|151=100
PARTIAL: 39=1|11=ORD001|14=50|151=150|31=151.00
FILLED:  39=2|11=ORD001|14=100|151=0|31=150.00
CANCEL:  39=4|11=ORD001|14=0|151=0
```

Tag meanings:
- `39` — order status (0=new, 1=partial, 2=filled, **4**=cancelled)
- `14` — cumQty: total filled across all fills
- `151` — leavesQty: shares still open
- `31` — lastPx: price of the most recent fill

**Why status code 4 and not 3 for CANCELLED?** Real FIX protocol — code 3 means "Done For Day", a different concept. Cancelled is specifically 4.

**`std::ostringstream` for string building:**
```cpp
std::ostringstream ss;
ss << "39=" << statusCode << "|11=" << event.orderId;
return ss.str();
```
Cleaner than `+` concatenation — mixes strings, ints, and doubles without manual conversion.

**`std::fixed << std::setprecision(2)`** — formats doubles to exactly 2 decimal places:
```
without: 150.1    (unpredictable)
with:    150.10   (always 2 dp)
```

**Append-only `std::vector`** — `push_back` is amortised O(1). We never insert or delete in the middle, so vector is the right container here. In production this would be flushed asynchronously to a database by a separate process (that's our consumer service).

---

### `src/main.cpp`

Wires everything together. Owns the TCP sockets and the main processing loop.

**Winsock initialisation (Windows only):**
```cpp
WSADATA wsa;
WSAStartup(MAKEWORD(2, 2), &wsa);  // must call before any socket operation
// ...
WSACleanup();                       // release at end
```
On Linux you use POSIX sockets directly with no initialisation. On Windows, Winsock must be initialised once per process.

**`bindAndListen` helper:**
```cpp
SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, ...);  // reuse port immediately after restart
addr.sin_addr.s_addr = INADDR_ANY;               // accept on any network interface
addr.sin_port = htons(port);                     // host → network byte order (big-endian)
bind(srv, ...);
listen(srv, 1);                                  // backlog 1 — one connection per port
```

**`htons()`** — "host to network short." TCP/IP requires big-endian byte order for port numbers. Your CPU is little-endian. `htons` does the conversion.

**`SO_REUSEADDR`** — without it, after the engine crashes and restarts, the OS holds the port in TIME_WAIT state for ~60 seconds before releasing it. This flag bypasses that.

**Connection order matches startup sequence:**
```cpp
SOCKET consumerConn = accept(consumerSrv, nullptr, nullptr);  // blocks until consumer connects
SOCKET gatewayConn  = accept(gatewaySrv,  nullptr, nullptr);  // then blocks until gateway connects
```
`accept()` is blocking. Engine waits for consumer first (port 7002), then gateway (port 7001) — matching the mandatory startup order from the design doc.

**TCP receive buffer — the most important pattern:**
```cpp
std::string buffer;
char chunk[4096];

while (true) {
    int bytes = recv(gatewayConn, chunk, sizeof(chunk) - 1, 0);
    buffer += chunk;

    size_t pos;
    while ((pos = buffer.find('\n')) != std::string::npos) {
        std::string line = buffer.substr(0, pos);
        buffer.erase(0, pos + 1);
        // process line
    }
}
```

TCP is a **stream protocol** — there are no message boundaries. One `recv` call might return half a line, a full line, or three lines at once depending on network timing. You must buffer raw bytes and only process complete lines (terminated by `\n`). This is the standard pattern for all newline-delimited TCP protocols.

**Main processing dispatch:**
```cpp
if (msg.type == MessageType::CANCEL) {
    book.cancelOrder(msg.cancelId);
    sendLine(gatewayConn, "39=4|11=" + msg.cancelId + "|14=0|151=0");

} else if (msg.type == MessageType::NEW_ORDER) {
    auto trades = engine.processOrder(msg.order);
    log.record(msg.order, trades.empty() ? 0.0 : trades.back().price);
    sendLine(gatewayConn, log.formatExecutionReport(log.getEvents().back()));
    for (const auto& trade : trades) {
        sendLine(consumerConn, formatTrade(trade));
    }
}
```
One execution report → gateway. Each fill event → consumer. Execution report always sent first.

**`formatTrade` output:**
```
TRADE|buyOrderId=ORD001|sellOrderId=ORD003|price=150.00|qty=100|ts=1704067200000001500
```

---

## Build Commands

```bash
# Configure (run once, or when CMakeLists.txt changes)
cmake -S . -B build -G "MinGW Makefiles"

# Build (run every time a .cpp or .h file changes)
cmake --build build --parallel 8

# Run
./build/orderbook.exe
```

**CMakeLists.txt — final state:**
```cmake
cmake_minimum_required(VERSION 3.20)
project(orderbook VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

add_executable(orderbook
    src/main.cpp
    src/OrderBook.cpp
    src/MatchingEngine.cpp
    src/Parser.cpp
    src/EventLog.cpp
)

target_link_libraries(orderbook ws2_32)
```

`target_link_libraries(orderbook ws2_32)` — links the Windows socket library. Without it the linker cannot find `socket`, `bind`, `accept`, `recv`, `send`, etc.

---

## Test Scenarios — Step by Step

Run `python tests/test_engine.py` while engine is running.

---

### Scenario 1 — LIMIT orders that rest

```
Send: LIMIT BUY  100 @ $150.00 (ORD001)
Send: LIMIT SELL 100 @ $151.00 (ORD002)
```

Best bid ($150.00) < best ask ($151.00) — the gap is called the **spread**. No cross, no trade. Both orders rest.

```
After:  ASKS: {151.00: [ORD002]}
        BIDS: {150.00: [ORD001]}
```
Both report `39=0` (ACK).

---

### Scenario 2 — Crossing LIMIT → full fill

```
Send: LIMIT SELL 100 @ $150.00 (ORD003)
```
Incoming SELL asks for at least $150.00. Best bid offers exactly $150.00. Prices cross. Fill executes at the **resting order's price** ($150.00 — the buyer's price is honoured).

```
Fill: ORD001(buy) vs ORD003(sell), qty=100, price=$150.00
After: ASKS: {151.00: [ORD002]},  BIDS: {}
```
ORD003 reports `39=2` (FILLED). Trade event emitted to consumer.

---

### Scenario 3 — MARKET order

```
Send: MARKET BUY 50 (ORD004)
```
No price field. No price check. Matches at whatever the best ask is ($151.00).

```
Fill: ORD004(buy) vs ORD002(sell, 100 shares), qty=50, price=$151.00
After: ASKS: {151.00: [ORD002(50 left)]},  BIDS: {}
```
ORD004 reports `39=2` (FILLED). ORD002 partially filled, 50 shares remain.

---

### Scenario 4 — Partial fill, remainder rests

```
Send: LIMIT BUY 200 @ $152.00 (ORD005)
```
Crosses with ORD002 (50 remaining). Only 50 available. 50 fills, 150 unfilled. Since it's LIMIT, remainder rests.

```
Fill: ORD005(buy) vs ORD002(sell, 50 left), qty=50, price=$151.00
After: ASKS: {},  BIDS: {152.00: [ORD005(150 left)]}
```
ORD005 reports `39=1` (PARTIAL). 150 shares now resting in book.

---

### Scenario 5 — IOC order

```
Send: IOC SELL 300 @ $152.00 (ORD006)
```
Crosses with ORD005 (150 shares at $152.00). Fills 150. 150 remain — but IOC never rests. Remainder cancelled immediately.

```
Fill: ORD005(buy) vs ORD006(sell), qty=150, price=$152.00
After: ASKS: {},  BIDS: {}
```
ORD006 reports `39=4` (CANCELLED — IOC remainder), cumQty=150, leavesQty=150. Trade event emitted.

IOC exists because algorithmic traders want to take liquidity without leaving a visible resting order in the book.

---

### Scenario 6 — Cancel a resting order

```
Send: LIMIT BUY 500 @ $148.00 (ORD007)  →  ACK, rests
Send: CANCEL ORD007
```
`orderIndex` finds ORD007 in O(1). Marks it CANCELLED in-place (tombstone). Removes pointer from index. Order stays in deque physically but is invisible to the system.

```
After: BIDS: {148.00: [ORD007(CANCELLED tombstone)]}
```
Cancel reports `39=4|11=ORD007|14=0|151=0`.

---

### Scenario 7 — MARKET order with empty book

```
Send: MARKET SELL 100 (ORD008)
```
Engine checks bids. Finds $148.00 price level with ORD007. ORD007 is a tombstone — tombstone cleanup runs, deque becomes empty, price level erased. Bids side now truly empty. MARKET order has no price to rest at → cancelled.

```
After: ASKS: {},  BIDS: {}
```
Reports `39=4|11=ORD008|14=0|151=100` — nothing filled, whole order cancelled.

---

## Order Type Summary

```
Scenario          | Order Type | Result
──────────────────|────────────|─────────────────────────────────
No counterpart    | LIMIT      | Rests in book at limit price
No counterpart    | MARKET     | Cancelled immediately
No counterpart    | IOC        | Cancelled immediately
Full match        | any        | FILLED
Partial match     | LIMIT      | PARTIAL, remainder rests
Partial match     | MARKET     | FILLED (uses all available), remainder cancelled
Partial match     | IOC        | CANCELLED (final state), remainder cancelled
Cancel request    | —          | Tombstone set, removed from index
```

---

## Key C++ Concepts Encountered

| Concept | Where used | What it does |
|---|---|---|
| `#pragma once` | All headers | Prevents double-include errors |
| `enum class` | Order.h | Scoped enums — no name leaks, no implicit int conversion |
| `auto` | OrderBook.cpp, MatchingEngine.cpp | Compiler infers type — avoids verbose iterator types |
| `->` operator | OrderBook.cpp | Dereference pointer and access member (`(*ptr).field`) |
| Member initialiser list | MatchingEngine.cpp | Required for initialising reference members |
| `std::prev(end())` | MatchingEngine.cpp | Forward iterator to last map element (for erase compatibility) |
| `std::ostringstream` | EventLog.cpp | String builder for mixed types |
| `std::fixed` / `std::setprecision` | EventLog.cpp | Consistent decimal formatting for prices |
| `htons()` | main.cpp | Host-to-network byte order for port numbers |
| `SO_REUSEADDR` | main.cpp | Allows port reuse immediately after restart |
| TCP stream buffering | main.cpp | Buffer + find('\n') pattern for newline-delimited protocol |
| Tombstone pattern | OrderBook.cpp | Safe cancel without deque pointer invalidation |
