# Orderbook System

A production-inspired order book trading system built across five services. Orders flow from a Java client through a Spring Boot gateway, get matched by a C++17 engine using price-time priority, and the resulting trades are persisted to a cloud MySQL database. A separate market-data service broadcasts live order book snapshots after every order.

Built as a portfolio project targeting fintech software engineering roles.

---

## Architecture

```
                    ┌─────────────────────────────────────────────────────┐
                    │               C++17 Matching Engine                 │
                    │                                                     │
Java Client ──HTTP──▶ Gateway   ──TCP:7001──▶  price-time priority match │
            POST       (8080)                  std::map / std::deque      │
            /orders    WAL                     FIX protocol               │
                    │       ◀──execution report──                         │
                    │                    │                                │
                    │                    ├──TCP:7002──▶ Consumer (8081)   │
                    │                    │              TiDB Cloud MySQL  │
                    │                    │                                │
                    │                    └──TCP:7003──▶ Market Data (8082)│
                    │                                  GET /book          │
                    └─────────────────────────────────────────────────────┘
```

| Service | Language | Port | Role |
|---------|----------|------|------|
| engine | C++17 | 7001, 7002, 7003 | In-memory matching engine |
| gateway | Spring Boot 3.3.5 | 8080 | Validates orders, WAL, forwards to engine |
| consumer | Spring Boot 3.3.5 | 8081 | Persists trade events to TiDB Cloud |
| market-data | Spring Boot 3.3.5 | 8082 | Broadcasts live order book snapshots |
| client | Plain Java 22 | — | Simulates order flow |

---

## Tech Stack

- **C++17** — matching engine, Winsock2 TCP, `std::map`/`std::deque` order book
- **Java 22 / Spring Boot 3.3.5** — gateway, consumer, market-data services
- **FIX Protocol** — order/execution message format between gateway and engine
- **TiDB Cloud** — serverless MySQL 8.0 compatible cloud database
- **Maven** — build system for all Java services
- **CMake + MinGW** — build system for the C++ engine

---

## Prerequisites

- GCC 14.2.0 (MinGW-w64)
- CMake 3.20+
- Java 22
- Maven 3.9+
- Python 3 (for fake consumer during benchmarking)

---

## Getting Started

### 1. Build the engine

```bash
cd engine
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build --parallel 8
```

### 2. Configure the consumer

Copy the example and fill in your TiDB Cloud credentials:

```bash
cp consumer/src/main/resources/application.properties.example \
   consumer/src/main/resources/application.properties
```

Edit `application.properties` with your TiDB Cloud connection details.

### 3. Start services in order

The engine blocks until all three downstream services connect — start them in this sequence:

```bash
# Terminal 1
cd engine/build && ./orderbook.exe

# Terminal 2
cd consumer && mvn spring-boot:run

# Terminal 3
cd market-data && mvn spring-boot:run

# Terminal 4
cd gateway && mvn spring-boot:run
```

### 4. Run the client simulator

```bash
cd client
mvn package -q
java -jar target/client-0.0.1-SNAPSHOT.jar
```

---

## API Reference

### Gateway — POST /orders

```bash
curl -X POST http://localhost:8080/orders \
  -H "Content-Type: application/json" \
  -d '{"orderId":"ORD001","side":"BUY","quantity":100,"price":150.00,"type":"LIMIT"}'
```

Supported types: `LIMIT`, `MARKET`, `IOC`. MARKET orders omit the `price` field.

### Gateway — DELETE /orders/{orderId}

```bash
curl -X DELETE http://localhost:8080/orders/ORD001
```

### Consumer — GET /trades

```bash
curl http://localhost:8081/trades
curl http://localhost:8081/trades/ORD001
```

### Market Data — GET /book

```bash
curl http://localhost:8082/book
```

---

## Scenario Tests

All scenarios run against a clean book. Each demonstrates a distinct order book behavior.

---

### Scenario 1 — Resting orders, no fill

A BUY and SELL that don't cross — spread between them means no match. Both orders enter the book and wait.

```bash
curl -s -X POST http://localhost:8080/orders \
  -H "Content-Type: application/json" \
  -d '{"orderId":"S1-BUY","side":"BUY","quantity":100,"price":149.00,"type":"LIMIT"}'

curl -s -X POST http://localhost:8080/orders \
  -H "Content-Type: application/json" \
  -d '{"orderId":"S1-SELL","side":"SELL","quantity":100,"price":151.00,"type":"LIMIT"}'

curl -s http://localhost:8082/book | python -m json.tool
```

```json
{
    "bids": [{ "price": 149.0, "quantity": 100 }],
    "asks": [{ "price": 151.0, "quantity": 100 }]
}
```

149 < 151 — no cross. Spread = $2.00. Both orders sit in the book waiting for a counterpart.

---

### Scenario 2 — Exact fill

A SELL arrives at the exact bid price and quantity — fully consumed in one match.

```bash
curl -s -X POST http://localhost:8080/orders \
  -H "Content-Type: application/json" \
  -d '{"orderId":"S2-SELL","side":"SELL","quantity":100,"price":149.00,"type":"LIMIT"}'
```

```json
{"orderId":"S2-SELL","status":"FILLED","cumQty":100,"leavesQty":0,"lastPx":149.0}
```

```bash
curl -s http://localhost:8082/book | python -m json.tool
```

```json
{
    "bids": [],
    "asks": [{ "price": 151.0, "quantity": 100 }]
}
```

S1-BUY fully consumed. Trade executes at 149.00 — the resting bid's price, not the aggressive order's.

---

### Scenario 3 — Partial fill

An aggressive BUY larger than the resting ask quantity. Fills what's available, remainder enters the book.

```bash
curl -s -X POST http://localhost:8080/orders \
  -H "Content-Type: application/json" \
  -d '{"orderId":"S3-BUY","side":"BUY","quantity":150,"price":151.00,"type":"LIMIT"}'
```

```json
{"orderId":"S3-BUY","status":"PARTIAL","cumQty":100,"leavesQty":50,"lastPx":151.0}
```

```bash
curl -s http://localhost:8082/book | python -m json.tool
```

```json
{
    "bids": [{ "price": 151.0, "quantity": 50 }],
    "asks": []
}
```

S1-SELL consumed entirely. Remaining 50 shares of S3-BUY rest in the book as a new bid at 151.00.

---

### Scenario 4 — Multi-level sweep

One large aggressive BUY sweeps through three ask price levels in a single order, generating three separate trade records.

```bash
curl -s -X POST http://localhost:8080/orders -H "Content-Type: application/json" \
  -d '{"orderId":"S4-SELL1","side":"SELL","quantity":30,"price":152.00,"type":"LIMIT"}'
curl -s -X POST http://localhost:8080/orders -H "Content-Type: application/json" \
  -d '{"orderId":"S4-SELL2","side":"SELL","quantity":30,"price":153.00,"type":"LIMIT"}'
curl -s -X POST http://localhost:8080/orders -H "Content-Type: application/json" \
  -d '{"orderId":"S4-SELL3","side":"SELL","quantity":30,"price":154.00,"type":"LIMIT"}'
```

Book before sweep:
```json
{
    "bids": [{ "price": 151.0, "quantity": 50 }],
    "asks": [
        { "price": 152.0, "quantity": 30 },
        { "price": 153.0, "quantity": 30 },
        { "price": 154.0, "quantity": 30 }
    ]
}
```

```bash
curl -s -X POST http://localhost:8080/orders -H "Content-Type: application/json" \
  -d '{"orderId":"S4-BUY","side":"BUY","quantity":90,"price":155.00,"type":"LIMIT"}'
```

```json
{"orderId":"S4-BUY","status":"FILLED","cumQty":90,"leavesQty":0,"lastPx":154.0}
```

```bash
curl -s http://localhost:8081/trades/S4-BUY | python -m json.tool
```

```json
[
    {"buyOrderId":"S4-BUY","sellOrderId":"S4-SELL1","price":152.0,"quantity":30},
    {"buyOrderId":"S4-BUY","sellOrderId":"S4-SELL2","price":153.0,"quantity":30},
    {"buyOrderId":"S4-BUY","sellOrderId":"S4-SELL3","price":154.0,"quantity":30}
]
```

Price-time priority: cheapest ask filled first. S4-BUY paid a different price for each tranche.

---

### Scenario 5 — IOC partial fill

IOC (Immediate Or Cancel) fills whatever is available instantly and cancels the remainder. Never rests in the book.

```bash
curl -s -X POST http://localhost:8080/orders -H "Content-Type: application/json" \
  -d '{"orderId":"S5-SELL","side":"SELL","quantity":50,"price":152.00,"type":"LIMIT"}'

curl -s -X POST http://localhost:8080/orders -H "Content-Type: application/json" \
  -d '{"orderId":"S5-IOC","side":"BUY","quantity":200,"price":155.00,"type":"IOC"}'
```

```json
{"orderId":"S5-IOC","status":"CANCELLED","cumQty":50,"leavesQty":150}
```

Filled 50 shares, cancelled the remaining 150 immediately. `GET /book` shows no IOC remainder on the bid side.

---

### Scenario 6 — MARKET order

MARKET orders carry no price — the engine matches at the best available price on the opposite side.

```bash
curl -s -X POST http://localhost:8080/orders \
  -H "Content-Type: application/json" \
  -d '{"orderId":"S6-MKT","side":"SELL","quantity":30,"type":"MARKET"}'
```

```json
{"orderId":"S6-MKT","status":"FILLED","cumQty":30,"leavesQty":0,"lastPx":151.0}
```

Filled at 151.00 — the best available bid. No price field in the request. Bid quantity dropped from 50 to 20.

---

### Scenario 7 — Cancel a resting order

Cancel a resting order and verify it disappears from the book.

```bash
curl -s -X DELETE http://localhost:8080/orders/S3-BUY
curl -s http://localhost:8082/book | python -m json.tool
```

```json
{"orderId":"S3-BUY","status":"CANCELLED","cumQty":0,"leavesQty":0}
```

```json
{ "bids": [], "asks": [] }
```

Order removed. Book fully empty.

---

## Benchmark

The benchmark connects directly to the engine on port 7001, bypassing the gateway. It pre-generates orders in memory with a fixed seed for reproducibility, then sends them as fast as possible measuring throughput and latency.

```bash
cd engine/build && ./bench.exe
```

Results across three scale runs (fixed seed 42, LIMIT orders only, price range 145–155):

| Orders | Throughput | Avg latency | Fill rate |
|--------|-----------|-------------|-----------|
| 10,000 | ~10,000/sec | 0.091ms | 37.9% |
| 100,000 | 10,482/sec | 0.095ms | 37.1% |
| 1,000,000 | 10,642/sec | 0.094ms | 36.9% |

**Throughput is flat across 100x scale.** The `std::map` depth is bounded by the number of distinct price levels (~1,000 at 0.01 granularity over a ±5 range), not by order count. Every lookup is O(log 1,000) ≈ 10 comparisons regardless of total orders processed.

The dominant cost at all scales is TCP loopback round-trip (~0.094ms), not matching logic. The engine itself operates in nanoseconds.

---

## What I'd Improve

- **Order book recovery** — if the engine crashes, the in-memory book is lost. A production system would snapshot the book periodically and replay from the last snapshot + WAL tail on restart.
- **In-process benchmark** — the current benchmark measures end-to-end TCP latency. An in-process benchmark calling `engine.processOrder()` directly would isolate raw matching throughput, expected to be 50-100x higher.
- **Market data feed to client** — the client simulator currently generates random prices. A smarter client would read `GET /book` first and price orders relative to the current spread.
- **Concurrent accept** — the engine's sequential `accept()` means all downstream services must start in exact order. Production systems handle reconnects concurrently.
- **Authentication** — any TCP client can connect to the engine on port 7001. A real system would authenticate connections.
