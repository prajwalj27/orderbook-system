# 07 — Market Data Service Implementation

The market-data service is the fifth and final service in the system. It receives a live broadcast of the order book state from the engine after every order is processed and exposes the current snapshot via a REST endpoint. This is what allows traders to see the current bid/ask prices before deciding where to price their orders.

---

## Why This Service Exists

Before this service, nobody outside the engine knew what was in the order book. The gateway only received execution reports for its own orders. The consumer only received trade events. There was no way to answer the question: "what is the current best bid and ask?"

In real exchanges this is called the **market data feed** — a continuous broadcast of the order book state to all market participants. Bloomberg terminals, algorithmic trading systems, and brokerage apps all consume a market data feed to price their orders intelligently.

---

## Architecture

```
Engine (port 7003) ──TCP broadcast──▶ BookListener ──▶ AtomicReference<BookSnapshot>
                                                                  ▲
HTTP client ──────────────────────────────▶ BookController ───────┘
                                           GET /book (8082)
```

The key difference from the consumer: there is no database. The market data feed only needs the **latest** snapshot — historical book state has no value. The snapshot is held in memory using an `AtomicReference` and overwritten on every update.

---

## Engine Changes

### OrderBook.h — BookSnapshot and PriceLevel structs

```cpp
struct PriceLevel {
    double price;
    int    quantity;   // total remaining qty at this level
};

struct BookSnapshot {
    std::vector<PriceLevel> bids;  // best (highest) first
    std::vector<PriceLevel> asks;  // best (lowest) first
};
```

These structs live in `OrderBook.h` because they describe the book's public state. `PriceLevel` represents one row in the book — a price and the total remaining quantity at that price. `BookSnapshot` is up to 5 bids and 5 asks.

### OrderBook.cpp — getSnapshot()

```cpp
BookSnapshot OrderBook::getSnapshot(int depth) const {
    // bids — iterate from highest price downward
    auto bit = bids.rbegin();
    while (bit != bids.rend() && snap.bids.size() < depth) {
        int totalQty = 0;
        for (const auto& o : bit->second)
            if (o.status != OrderStatus::CANCELLED)
                totalQty += (o.quantity - o.filledQty);
        if (totalQty > 0) snap.bids.push_back({bit->first, totalQty});
        ++bit;
    }
    // asks — iterate from lowest price upward (same pattern)
}
```

**Reverse iterator for bids** — `bids` is a `std::map<double, deque>` sorted ascending. The best bid is the highest price — `rbegin()` starts there and walks down. The best ask is the lowest price — `asks.begin()` starts there and walks up.

**Skipping cancelled/filled orders** — the engine uses tombstone cancellation (orders marked `CANCELLED` in place, not removed). When computing the quantity at a level, only orders that are not cancelled and have remaining quantity (`quantity - filledQty > 0`) are counted. This ensures the snapshot reflects actual available liquidity, not ghost orders.

**Skipping empty levels** — if all orders at a price level are cancelled or filled, `totalQty` will be 0 and that level is not included in the snapshot.

### main.cpp — formatSnapshot() and broadcast

```cpp
static std::string formatSnapshot(const BookSnapshot& snap) {
    // produces: BOOK|bids=150.00x100,149.50x80|asks=151.00x60,152.00x40
}
```

The snapshot is serialized as a pipe-delimited string. Each price level is `price x quantity` separated by commas within the bids/asks fields. This format is compact, human-readable, and easy to parse.

The broadcast happens after **every order** — both new orders and cancels:

```cpp
// broadcast book snapshot after every order
sendLine(marketDataConn, formatSnapshot(book.getSnapshot(5)));
```

This means `GET /book` always reflects the state of the book after the most recently processed order.

### Startup order change

The engine now blocks on three `accept()` calls in sequence:

```
port 7002 → consumer connects
port 7003 → market-data connects
port 7001 → gateway connects → engine ready
```

The gateway must connect last because it is the only one that sends orders. The consumer and market-data must be connected first so no trade or book snapshot is sent to a disconnected socket.

---

## market-data Service

### PriceLevel.java

Simple Lombok data class — price and quantity. Maps directly to the C++ `PriceLevel` struct.

### BookSnapshot.java

```java
public double bestBid()  { return bids.isEmpty()  ? 0.0 : bids.get(0).getPrice(); }
public double bestAsk()  { return asks.isEmpty()  ? 0.0 : asks.get(0).getPrice(); }
public double spread()   { return bestAsk() - bestBid(); }
```

Three helper methods added beyond the Lombok data fields. `bestBid()` and `bestAsk()` return the top-of-book prices. `spread()` returns the gap between them — zero if either side is empty. These are the three numbers traders watch most closely.

### BookListener.java

Same structural pattern as `EngineListener` in the consumer — daemon thread, retry loop, `@PostConstruct`/`@PreDestroy`. The key difference is what happens with each message:

**Consumer** saves to database — durable, historical.
**BookListener** overwrites in memory — ephemeral, always current.

```java
private final AtomicReference<BookSnapshot> latest = new AtomicReference<>(emptySnapshot());

while (running && (line = in.readLine()) != null) {
    BookSnapshot snap = parseSnapshot(line.trim());
    if (snap != null) latest.set(snap);
}
```

**`AtomicReference`** is used instead of `synchronized` because the operation is a single reference swap — the listener thread writes a new `BookSnapshot` object, the HTTP thread reads the current one. `AtomicReference.set()` and `get()` are both atomic operations with no locking overhead. If `synchronized` were used, every HTTP request to `GET /book` would block while the listener is updating, and vice versa.

**Parsing the snapshot message:**
```
BOOK|bids=150.00x100,149.50x80|asks=151.00x60,152.00x40
```

The `extractField()` method finds the value between `|key=` and the next `|`. `parseLevels()` splits by comma then by `x` to get price and quantity pairs.

### BookController.java

Single endpoint:

```java
@GetMapping
public BookSnapshot getBook() {
    return bookListener.getLatest();
}
```

`getLatest()` calls `AtomicReference.get()` — a single read with no locking. The response is always the latest snapshot the listener received. If no orders have been sent yet, it returns an empty snapshot (bids=[], asks=[]).

---

## Scenario Tests

All tests run against a clean book. Each shows the state before and after the relevant orders.

### Scenario 1 — Resting orders, no fill
```
POST BUY  S1-BUY  qty=100 price=149.00 LIMIT → NEW
POST SELL S1-SELL qty=100 price=151.00 LIMIT → NEW
GET /book → bids=[149.00x100]  asks=[151.00x100]  spread=$2.00
```
149 < 151 — no cross, both orders rest. Spread of $2.00 visible in the book.

### Scenario 2 — Exact fill
```
POST SELL S2-SELL qty=100 price=149.00 LIMIT → FILLED cumQty=100
GET /book → bids=[]  asks=[151.00x100]
GET /trades → [{buy:S1-BUY, sell:S2-SELL, price:149.00, qty:100}]
```
S2-SELL crossed the spread and consumed S1-BUY exactly. Bid side cleared. Trade at 149.00 — the resting bid's price.

### Scenario 3 — Partial fill
```
POST BUY S3-BUY qty=150 price=151.00 LIMIT → PARTIAL cumQty=100 leavesQty=50
GET /book → bids=[151.00x50]  asks=[]
```
S3-BUY consumed all 100 of S1-SELL then the remaining 50 entered the book as a resting bid. Ask side cleared.

### Scenario 4 — Multi-level sweep
```
POST SELL S4-SELL1 qty=30 price=152.00 LIMIT → NEW
POST SELL S4-SELL2 qty=30 price=153.00 LIMIT → NEW
POST SELL S4-SELL3 qty=30 price=154.00 LIMIT → NEW
GET /book → bids=[151.00x50]  asks=[152.00x30, 153.00x30, 154.00x30]

POST BUY S4-BUY qty=90 price=155.00 LIMIT → FILLED cumQty=90 lastPx=154.00
GET /book → bids=[151.00x50]  asks=[]
GET /trades/S4-BUY → 3 records at 152, 153, 154
```
One order swept three price levels. Each fill at the resting seller's price — cheapest first.

### Scenario 5 — IOC partial fill
```
POST SELL S5-SELL qty=50 price=152.00 LIMIT → NEW
POST BUY  S5-IOC  qty=200 price=155.00 IOC  → CANCELLED cumQty=50 leavesQty=150
GET /book → bids=[151.00x50]  asks=[]
```
IOC filled 50 shares immediately, cancelled the remaining 150. Never entered the book.

### Scenario 6 — MARKET order
```
POST SELL S6-MKT qty=30 type=MARKET → FILLED cumQty=30 lastPx=151.00
GET /book → bids=[151.00x20]  asks=[]
```
No price field. Engine matched at best available bid (151.00). Bid quantity dropped from 50 to 20. Trade shows S3-BUY as the passive buyer — it was the resting order.

### Scenario 7 — Cancel
```
DELETE /orders/S3-BUY → CANCELLED
GET /book → bids=[]  asks=[]
```
Remaining 20 shares of S3-BUY cancelled. Book fully empty.

---

## application.properties

```properties
server.port=8082
engine.host=localhost
engine.port.marketdata=7003
```

No database credentials needed — this service holds no persistent state.
