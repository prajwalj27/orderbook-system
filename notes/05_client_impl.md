# 05 — Java Client Implementation

The client is a plain Java 22 application — no Spring Boot, no web server, just a `main()` method. It simulates a trading firm's order flow by sending randomized HTTP orders to the gateway and printing each response.

---

## Architecture

```
ClientSimulator.main()
    └── java.net.http.HttpClient
            └── POST http://localhost:8080/orders  (gateway)
```

The client only talks to the gateway. It has no direct connection to the engine or consumer. From the client's perspective, the rest of the system is a black box — it sends an order and gets a status back.

---

## pom.xml

The client does not use Spring Boot as a parent. It is a plain Maven project with `maven-jar-plugin` configured to embed the main class in the JAR manifest. This means you run it with:

```bash
mvn package -q
java -jar target/client-0.0.1-SNAPSHOT.jar
```

No extra dependencies are needed. `java.net.http.HttpClient` is part of the Java standard library since Java 11, so the client can send HTTP requests without Jackson, OkHttp, or any third-party library.

---

## ClientSimulator.java

### Configuration constants

```java
private static final double REFERENCE_PRICE = 150.00;
private static final double PRICE_SPREAD    = 5.00;
private static final int    ORDER_COUNT     = 20;
private static final int    DELAY_MS        = 300;
```

- `REFERENCE_PRICE` — the midpoint around which all LIMIT and IOC prices are generated. Mirrors the `risk.reference-price` in the gateway so most orders pass the price collar check.
- `PRICE_SPREAD` — prices are randomly generated in the range `[145.00, 155.00]`. The ±5 spread means BUY and SELL orders will frequently cross each other's prices, creating fills.
- `DELAY_MS` — 300ms between orders. This simulates real order flow and gives the engine time to process each order before the next arrives.

### Order type weighting

```java
private static final String[] TYPES = {"LIMIT", "LIMIT", "LIMIT", "MARKET", "IOC"};
```

LIMIT appears three times out of five slots — 60% probability. MARKET and IOC each have 20%. This weighting is intentional: LIMIT orders rest in the book and create liquidity, while MARKET and IOC orders consume it. A realistic simulation needs more resting orders than aggressive ones, otherwise the book stays empty and nothing matches.

### HttpClient

```java
HttpClient client = HttpClient.newHttpClient();
HttpRequest request = HttpRequest.newBuilder()
        .uri(URI.create(GATEWAY_URL))
        .header("Content-Type", "application/json")
        .POST(HttpRequest.BodyPublishers.ofString(body))
        .build();
HttpResponse<String> response = client.send(request, HttpResponse.BodyHandlers.ofString());
```

`HttpClient.newHttpClient()` creates a client with default settings — HTTP/1.1, no timeout. `send()` is synchronous (blocking) — the loop waits for each response before sending the next order. This is intentional: it keeps the order sequence deterministic and easier to trace through the order book.

### JSON without a library

```java
private static String extractField(String json, String key) {
    String search = "\"" + key + "\":\"";
    int start = json.indexOf(search);
    ...
}
```

Rather than pulling in Jackson just to read one field from a flat response, `extractField` does a simple string search. This works because the gateway responses are simple flat objects with no nesting. The tradeoff is that it would break on nested JSON or unusual formatting — acceptable for a simulator with a known response shape.

### MARKET orders have no price field

```java
if ("MARKET".equals(type)) {
    return String.format(
        "{\"orderId\":\"%s\",\"side\":\"%s\",\"quantity\":%d,\"type\":\"%s\"}",
        orderId, side, quantity, type);
}
```

MARKET orders omit the `price` field entirely. The gateway's `OrderValidator` allows a null price for MARKET orders, and the engine's parser infers `OrderType::MARKET` when tag 44 is absent.

---

## Run output walkthrough

This section traces through the actual run that produced the 13 trades stored in TiDB Cloud. The trades table is the ground truth — it records every match the engine made.

### How trade price is determined

In this engine, the trade executes at the **resting order's price** — the order that was already sitting in the book. When an aggressive order (the incoming one) crosses the spread, it accepts whatever price the passive order was offering. This is standard price-time priority.

### Order book state — building liquidity

The first several orders that arrive with no counterpart simply rest in the book:

- A **SELL LIMIT** arriving with no resting BUYs above its ask price enters the ask side and waits.
- A **BUY LIMIT** arriving with no resting SELLs below its bid price enters the bid side and waits.
- A **MARKET** or **IOC** with nothing to match against is immediately cancelled — they never rest.

Once enough orders have accumulated on both sides, incoming orders start crossing the spread and generating trades.

### Trade 1 — first match of the run

```json
{"id":1, "buyOrderId":"SIM-0001", "sellOrderId":"SIM-0003", "price":146.27, "qty":20}
```

SIM-0001 was a BUY. SIM-0003 was a SELL LIMIT resting in the book at 146.27. When SIM-0001 arrived with a bid price at or above 146.27, the engine matched them. The trade price is 146.27 — SIM-0003's ask price, since it was the passive/resting order.

### SIM-0001 sweeps three sellers (trades 1, 4, 5)

```json
{"id":1,  "buyOrderId":"SIM-0001", "sellOrderId":"SIM-0003", "price":146.27, "qty":20}
{"id":4,  "buyOrderId":"SIM-0001", "sellOrderId":"SIM-0009", "price":146.27, "qty":60}
{"id":5,  "buyOrderId":"SIM-0001", "sellOrderId":"SIM-0010", "price":146.27, "qty":20}
```

SIM-0001 bought a total of 100 shares across three separate sellers — all at 146.27. This means SIM-0003, SIM-0009, and SIM-0010 all had asks at the same price level. The engine matched them in time order (price-time priority): SIM-0003 was first in the queue at 146.27, then SIM-0009, then SIM-0010.

Trades 4 and 5 are not contiguous with trade 1 in time — SIM-0009 and SIM-0010 arrived later as new SELL orders, and SIM-0001's remaining quantity was still resting in the book waiting to be filled.

### SIM-0006 BUY sweeps two price levels (trades 2, 3)

```json
{"id":2, "buyOrderId":"SIM-0006", "sellOrderId":"SIM-0002", "price":146.89, "qty":10}
{"id":3, "buyOrderId":"SIM-0006", "sellOrderId":"SIM-0005", "price":150.43, "qty":40}
```

SIM-0006 was a BUY LIMIT with a bid high enough to cross two different ask price levels. The engine's matching loop always takes the **cheapest ask first** — so SIM-0002 at 146.89 was consumed before SIM-0005 at 150.43. This is price-time priority: best price first, time order within the same price. The buyer ends up paying two different prices for different portions of their order.

### SIM-0008 SELL fills two buyers (trades 6, 7)

```json
{"id":6, "buyOrderId":"SIM-0011", "sellOrderId":"SIM-0008", "price":148.06, "qty":40}
{"id":7, "buyOrderId":"SIM-0013", "sellOrderId":"SIM-0008", "price":148.06, "qty":50}
```

SIM-0008 was a SELL that arrived and found resting BUYs at 148.06. It filled 40 shares against SIM-0011, then 50 shares against SIM-0013. Total sold: 90 shares. The sell swept the entire bid queue at that price level across two resting orders.

### SIM-0007 SELL fills three buyers across time (trades 8, 10, 11)

```json
{"id":8,  "buyOrderId":"SIM-0013", "sellOrderId":"SIM-0007", "price":149.59, "qty":50}
{"id":10, "buyOrderId":"SIM-0018", "sellOrderId":"SIM-0007", "price":149.59, "qty":20}
{"id":11, "buyOrderId":"SIM-0019", "sellOrderId":"SIM-0007", "price":149.59, "qty":10}
```

SIM-0007 was a SELL LIMIT resting in the book at 149.59. Three separate BUY orders arrived over time and each matched against it. SIM-0007 was the passive order in all three trades — it sat in the book while buyers came to it. This is what `PARTIAL` fill looks like from the engine's perspective: the resting order keeps shrinking as buyers consume pieces of it.

### SIM-0005 SELL appears in three trades (trades 3, 12, 13)

```json
{"id":3,  "buyOrderId":"SIM-0006",  "sellOrderId":"SIM-0005", "price":150.43, "qty":40}
{"id":12, "buyOrderId":"SIM-0019",  "sellOrderId":"SIM-0005", "price":150.43, "qty":10}
{"id":13, "buyOrderId":"SIM-0020",  "sellOrderId":"SIM-0005", "price":150.43, "qty":50}
```

SIM-0005 was a large SELL LIMIT at 150.43 that took 3 separate fills before being consumed. First SIM-0006 took 40 shares, then SIM-0019 took 10, then SIM-0020 took 50. Total sold: 100 shares.

### Summary of the run

| Orders sent | 20 |
|-------------|-----|
| Trades generated | 13 |
| Orders with no fill (resting in book at shutdown) | ~7 |

The 13 trade records represent 13 individual match events. Some orders appear in multiple trades (partial fills over time), and some appear in only one (immediate full fill). Orders that ended as `NEW` with `leavesQty > 0` are still sitting in the engine's order book — they would fill if a counterpart order arrived.

---

## How to run

```bash
# Build once
cd client && mvn package -q

# Run (engine + consumer + gateway must already be up)
java -jar target/client-0.0.1-SNAPSHOT.jar

# Check what the consumer persisted
curl -s http://localhost:8081/trades | python -m json.tool

# Check trades for a specific order
curl -s http://localhost:8081/trades/SIM-0007 | python -m json.tool
```
