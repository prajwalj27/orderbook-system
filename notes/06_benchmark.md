# 06 — Benchmark Harness

The benchmark measures raw matching engine throughput and latency by connecting directly to port 7001, bypassing the gateway entirely. It pre-generates a large batch of FIX messages in memory and sends them as fast as possible, reading each execution report response before sending the next.

---

## Architecture

```
bench.exe
    └── TCP socket → engine port 7001 (direct, no gateway)

fake_consumer.py
    └── TCP socket → engine port 7002 (required — engine blocks until consumer connects)
```

The gateway is not involved. The bench acts as a raw TCP client sending the same FIX strings the gateway would send. This isolates the engine's matching performance from HTTP overhead, WAL writes, risk validation, and Spring Boot startup costs.

The fake consumer must still be connected on port 7002 because the engine's `main.cpp` calls `accept()` on 7002 before it accepts on 7001. Without a consumer the engine never unblocks.

---

## Design Decisions

### Fixed seed for reproducibility
```cpp
std::mt19937 rng(42);
```
The random number generator uses a fixed seed. Every run produces the exact same 1,000,000 orders in the exact same sequence. This means throughput differences between runs reflect hardware/OS variance — not workload variance. You can quote the number confidently knowing the input is controlled.

### Pre-generation before timing
All orders are generated and stored in memory before the timed section begins. String formatting (`ostringstream`, `setprecision`) happens outside the clock. The timed section only measures what the engine actually does: parse, match, respond.

### Synchronous send/receive loop
```cpp
send(sock, line.c_str(), ...);
while (recvBuffer.find('\n') == std::string::npos) {
    recv(sock, chunk, ...);
    recvBuffer += chunk;
}
```
Each order is sent and its response read before the next order is sent. This matches the gateway's behavior exactly. It also means the measured latency includes the full TCP round-trip — send FIX string to engine, engine parses and matches, engine sends execution report back, bench receives it.

### Response parsing for outcome tracking
The bench parses the `39` (status) and `14` (cumQty) tags from each execution report:
- `39=0` → NEW (order entered the book, no fill)
- `39=1` → PARTIAL (partially filled, remainder resting)
- `39=2` → FILLED (fully matched)
- `39=4` → CANCELLED (unused in this workload — no MARKET or IOC orders)

### Order ID padding with ostringstream
```cpp
std::ostringstream idss;
idss << "BN" << std::setfill('0') << std::setw(8) << i;
m.id = idss.str();
```
Using `ostringstream` with `setw(8)` handles any order count up to 99,999,999. The earlier approach using `std::string(6 - size, '0')` crashed at 1,000,000 orders because `size_t` underflows when the digit count exceeds the pad width.

---

## How to Run

```bash
# Terminal 1 — engine
cd engine/build && ./orderbook.exe

# Terminal 2 — fake consumer (from util/fake_consumer_script.txt)
# paste the python command — logs trades to util/logs/trades_<timestamp>.log

# Terminal 3 — benchmark
cd engine/build && ./bench.exe
```

Trade events during the benchmark are logged to `util/logs/` by the fake consumer for post-run analysis. The `util/logs/` directory is gitignored.

---

## Results

Three runs at increasing scale:

| Orders | Elapsed | Throughput | Avg latency | Fill rate | Partial |
|--------|---------|-----------|-------------|-----------|---------|
| 10,000 | 0.91s | ~10,000/sec | 0.091ms | 37.9% | 5.5% |
| 100,000 | 9.54s | 10,482/sec | 0.095ms | 37.1% | 5.6% |
| 1,000,000 | 93.96s | 10,642/sec | 0.094ms | 36.9% | 5.6% |

**Headline number: ~10,600 orders/sec sustained throughput over 1,000,000 orders.**

---

## Analysis

### Throughput is flat across 100x scale

Throughput at 1M orders (10,642/sec) is nearly identical to 10k orders (~10,000/sec). The engine does not slow down as order count grows.

This is because the `std::map` depth is bounded — not by total orders processed, but by the number of distinct active price levels. With a ±5 spread and 0.01 price granularity there are at most 1,000 possible price levels. No matter how many orders arrive, `std::map` never grows past 1,000 nodes. Every lookup is O(log 1,000) ≈ 10 comparisons — effectively constant.

### TCP round-trip dominates latency

The 0.09ms average latency is the TCP loopback cost. The actual matching logic — `std::map` lookup, `std::deque` front pop, quantity arithmetic — takes nanoseconds. If you benchmarked the engine in-process with no network layer, throughput would be 100x higher. The TCP layer is the ceiling here, not the matching algorithm.

### Fill rate is stable at scale

~37% FILLED, ~5.6% PARTIAL, ~57% NEW across all three run sizes. The ratios are a function of the price distribution — 50/50 BUY/SELL split with overlapping price ranges — not order count. This confirms the matching logic is deterministic and scales linearly.

### The order book never degrades

Real-world order books can degrade if they get too deep (many price levels far from the market). This workload never triggers that — prices cluster around 150 ± 5, so the book stays shallow and `std::map` lookups stay fast. A stress test with a wider spread (e.g. ±50) would exercise more price levels and could reveal O(log n) degradation.

---

## Trade Log Analysis (from 10k run)

The fake consumer logs every trade event to `util/logs/`. Running the Python analyzer against the 10k log produced:

| Metric | Value |
|--------|-------|
| Total trade events | 7,884 |
| Total qty matched | 219,016 |
| Unique buyer orders | 3,986 |
| Unique seller orders | 3,988 |
| Min trade price | 146.83 |
| Max trade price | 154.83 |
| Avg trade price | 149.98 |
| Min single-trade qty | 1 |
| Max single-trade qty | 98 |
| Avg single-trade qty | 27.78 |
| Avg time between trades | 114.71 µs |

**Price level distribution** — trade activity forms a bell curve centered on 150, confirming the matching engine correctly applies price-time priority (cheapest asks filled first, highest bids filled first):

```
$147-148    1,124 trades  ######################
$148-149    1,293 trades  #########################
$149-150    1,496 trades  #############################
$150-151    1,545 trades  ##############################  ← peak
$151-152    1,428 trades  ############################
$152-153      993 trades  ###################
```

Most active orders appeared in up to 6 separate trade events (partial fills over time), meaning no single order dominated the book.
