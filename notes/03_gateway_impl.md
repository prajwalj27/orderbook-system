# Gateway Implementation — Spring Boot Service

## Responsibilities

The gateway sits between the outside world and the C++ engine. It owns:

1. **Validation** — reject bad orders before they touch the engine
2. **WAL** — persist every valid order to disk before forwarding
3. **Engine communication** — maintain a persistent TCP connection to engine port 7001
4. **WAL replay** — on startup, replay the WAL to the engine before accepting live orders
5. **REST API** — expose `POST /orders` and `DELETE /orders/{orderId}` to callers

```
curl POST /orders
      ↓
OrderController
      ↓
OrderValidator  →  reject 400 if invalid
      ↓
WalWriter       →  append to gateway/data/wal.log  (BEFORE engine)
      ↓
EngineClient    →  send FIX over TCP port 7001, read execution report
      ↓
return OrderResponse JSON
```

---

## `application.properties`

```properties
server.port=8080
wal.path=gateway/data/wal.log
engine.host=localhost
engine.port=7001
risk.reference-price=150.00
risk.price-collar-pct=10.0
risk.max-quantity=10000
```

All risk parameters are configurable here — reference price, collar percentage, and max quantity. Components read these via `@Value` injection rather than hardcoding.

**WAL path note:** `wal.path=gateway/data/wal.log` is a relative path. It resolves relative to wherever `mvn spring-boot:run` is invoked from. If you run from inside `gateway/`, the file lands at `gateway/gateway/data/wal.log` from the project root. This is functional but not ideal — an absolute path or a path relative to the project root would be cleaner.

---

## Lombok Setup

Added to `pom.xml` to eliminate boilerplate getter/setter code:

```xml
<dependency>
    <groupId>org.projectlombok</groupId>
    <artifactId>lombok</artifactId>
    <optional>true</optional>
</dependency>
```

Also added exclusion in the Spring Boot Maven plugin so Lombok is not packaged into the final jar — it is only needed at compile time.

VS Code requires the **Lombok Annotations Support** extension (`GabrielBB.vscode-lombok`) to resolve Lombok-generated methods in the IDE. `mvn compile` works without it.

---

## File-by-File Breakdown

---

### `OrderRequest.java`

Represents the JSON body of an incoming `POST /orders` request.

```java
@Data
@NoArgsConstructor
@AllArgsConstructor
public class OrderRequest {
    private String orderId;
    private String side;      // "BUY" or "SELL"
    private String type;      // "LIMIT", "MARKET", or "IOC"
    private Double price;     // null for MARKET orders
    private int    quantity;
}
```

**`Double` not `double` for price** — MARKET orders have no price, sending `null` in JSON. A primitive `double` cannot be `null`. The boxed `Double` object can, so Jackson maps a missing price field correctly.

**Lombok annotations:**
- `@Data` — generates getters, setters, `toString`, `equals`, `hashCode`
- `@NoArgsConstructor` — empty constructor required by Jackson for JSON deserialisation
- `@AllArgsConstructor` — constructor with all fields

---

### `OrderResponse.java`

Represents the JSON body returned to the caller after order processing.

```java
@Data
@NoArgsConstructor
@AllArgsConstructor
public class OrderResponse {
    private String orderId;
    private String status;    // "NEW", "PARTIAL", "FILLED", "CANCELLED", "REJECTED"
    private int    cumQty;
    private int    leavesQty;
    private Double lastPx;   // null for ACK and CANCEL events
    private String message;  // populated only for rejections

    public static OrderResponse rejected(String reason) {
        OrderResponse r = new OrderResponse();
        r.status  = "REJECTED";
        r.message = reason;
        return r;
    }
}
```

**`rejected()` static factory method** — creates a rejection response without requiring all fields. Returns a clean 400 response to the caller without throwing an exception.

---

### `WalWriter.java`

Append-only file log. Writes every validated order to disk before the engine sees it. Also tracks seen order IDs for duplicate detection.

```java
@Component
public class WalWriter {

    @Value("${wal.path}")
    private String walPath;

    private final Set<String> seenIds = new HashSet<>();

    @PostConstruct
    public void init() throws IOException {
        Path path = Paths.get(walPath);
        Files.createDirectories(path.getParent());

        if (Files.exists(path)) {
            try (BufferedReader reader = Files.newBufferedReader(path)) {
                String line;
                while ((line = reader.readLine()) != null) {
                    extractOrderId(line).ifPresent(seenIds::add);
                }
            }
        }
    }

    public synchronized void write(String fixMessage) throws IOException { ... }
    public boolean contains(String orderId) { return seenIds.contains(orderId); }
}
```

**Key concepts:**

**`@Component`** — registers this class as a Spring-managed bean. Spring creates one instance on startup and injects it wherever needed.

**`@Value("${wal.path}")`** — injects the value from `application.properties`. Spring resolves the `${}` at startup. Missing property = hard failure.

**`@PostConstruct`** — runs after Spring has created the bean and injected all `@Value` fields. Cannot use a constructor because `@Value` fields aren't populated yet at construction time. On init: creates the `data/` directory, loads existing order IDs from WAL into `seenIds`.

**`HashSet<String> seenIds`** — O(1) average lookup for duplicate detection. A `List` would be O(n) per check.

**`synchronized` on `write`** — Spring Boot handles concurrent HTTP requests. Without `synchronized`, two simultaneous requests could interleave writes and corrupt the WAL file.

**`new FileWriter(walPath, true)`** — the `true` flag opens in append mode. Combined with `writer.flush()`, this guarantees the data hits disk before the method returns. This is what makes it a true write-ahead log — crash between write and engine forward means the order is recoverable.

**`Optional<String>` from `extractOrderId`** — avoids null. `.ifPresent(seenIds::add)` adds to the set only if a value was found — no null check needed.

**WAL line format:**
```
1704067200000000000|35=D|11=ORD001|54=1|44=150.10|38=500
```
`extractOrderId` splits by `|` and finds the part starting with `11=`.

---

### `OrderValidator.java`

Runs five pre-trade risk checks on every incoming order.

```java
@Component
public class OrderValidator {

    @Value("${risk.reference-price}") private double referencePrice;
    @Value("${risk.price-collar-pct}") private double collarPct;
    @Value("${risk.max-quantity}")     private int    maxQuantity;

    @Autowired private WalWriter walWriter;

    public Optional<String> validate(OrderRequest req) { ... }
}
```

**Five checks in order:**

| Check | Rule |
|---|---|
| Required fields | orderId, side, type, quantity always required |
| Side validation | must be BUY or SELL |
| Price rules | LIMIT/IOC require price; MARKET must not have price |
| Price collar | limit price must be within ±collarPct% of referencePrice |
| Quantity limit | single order cannot exceed maxQuantity |
| Duplicate ID | order ID must not already exist in WAL |

**`@Autowired`** — dependency injection. Spring finds the `WalWriter` bean it already created and wires it in. `OrderValidator` doesn't create its own `WalWriter`, it receives one.

**`Optional<String>` return type** — either returns an error message or nothing:
- `Optional.of("some error")` — validation failed
- `Optional.empty()` — all checks passed

Forces the caller to handle both cases explicitly.

**Price collar logic:**
```
referencePrice = $150.00, collarPct = 10.0
collar = 150.00 × 0.10 = $15.00
valid range: [$135.00, $165.00]
```
Catches fat-finger errors — a trader typing $15.00 instead of $150.00 gets rejected here.

**Switch expression (Java 14+):**
```java
switch (type) {
    case "LIMIT", "IOC" -> { ... }  // multiple labels, no break needed
    case "MARKET"       -> { ... }
    default             -> { ... }
}
```

---

### `EngineClient.java`

Owns the persistent TCP connection to the engine on port 7001.

```java
@Component
public class EngineClient {

    @PostConstruct
    public void connect() throws IOException { ... }

    public synchronized OrderResponse sendRaw(String fixMessage) throws IOException { ... }
    public synchronized OrderResponse send(OrderRequest req) throws IOException { ... }
    public synchronized OrderResponse cancel(String orderId) throws IOException { ... }

    @PreDestroy
    public void disconnect() throws IOException { ... }

    String toFix(OrderRequest req) { ... }           // package-private
    private OrderResponse parseResponse(String raw) { ... }
}
```

**`@PostConstruct` fail-fast:**
```java
} catch (IOException e) {
    throw new IllegalStateException("Cannot connect to engine...", e);
}
```
If the engine isn't running, the gateway refuses to start entirely. A gateway without an engine is useless — fail fast is the correct behaviour.

**`@PreDestroy`** — runs just before Spring shuts the bean down (Ctrl+C). Closes the socket cleanly.

**`BufferedWriter` + explicit `flush()`** — `PrintWriter.println()` uses the system line separator which on Windows is `\r\n`. The C++ engine looks for `\n` and would receive a trailing `\r` causing parse failures. Writing `"\n"` explicitly and calling `flush()` avoids this cross-platform issue.

**`synchronized` on `send`/`sendRaw`/`cancel`** — send, block on `readLine()`, return response. If two threads called simultaneously, responses would get crossed. `synchronized` ensures one order at a time.

**`sendRaw(String fixMessage)`** — added so `OrderController` can build the FIX string once (for the WAL) and pass it directly to the engine without rebuilding it.

**`toFix` is package-private (no access modifier)** — `OrderController` needs to call it to build the WAL entry. Private would prevent this. Package-private allows access within the same package without exposing it to the world.

**FIX encoding:**
```
LIMIT BUY:   35=D|11=ORD001|54=1|38=100|44=150.00   (tag 40 omitted for LIMIT)
MARKET SELL: 35=D|11=ORD002|54=2|38=50|40=1
IOC BUY:     35=D|11=ORD003|54=1|38=200|44=150.00|40=3
CANCEL:      35=F|11=ORD001
```

**`parseResponse`** — splits the execution report by `|`, builds a tag map, maps status code to string:
```
"0" → "NEW", "1" → "PARTIAL", "2" → "FILLED", "4" → "CANCELLED"
```

---

### `OrderController.java`

Wires validator, WAL writer, and engine client into the REST endpoints.

```java
@RestController
@RequestMapping("/orders")
public class OrderController {

    @PostMapping
    public ResponseEntity<OrderResponse> placeOrder(@RequestBody OrderRequest req) { ... }

    @DeleteMapping("/{orderId}")
    public ResponseEntity<OrderResponse> cancelOrder(@PathVariable String orderId) { ... }
}
```

**Request flow for `POST /orders`:**
```
1. validator.validate(req)        → return 400 if invalid
2. engineClient.toFix(req)        → build FIX string
3. walWriter.write(fix)           → persist to disk FIRST
4. engineClient.sendRaw(fix)      → send to engine, get execution report
5. return 200 with OrderResponse
```

**WAL written before engine** — this is the definition of write-ahead. If the process crashes after step 3 but before step 4, the order is in the WAL and will replay on restart. If we sent to the engine first and crashed before the WAL write, the order would be lost.

**`@RequestMapping("/orders")`** — base path for all endpoints in this controller.

**`@RequestBody OrderRequest req`** — Jackson deserialises the JSON body into an `OrderRequest` object automatically.

**`@PathVariable String orderId`** — extracts `{orderId}` from the URL. `DELETE /orders/ORD004` → `orderId = "ORD004"`.

**`ResponseEntity<OrderResponse>`** — controls HTTP status code alongside the body:
```java
ResponseEntity.ok(body)                // 200 OK
ResponseEntity.badRequest().body(...)  // 400 Bad Request
ResponseEntity.internalServerError()   // 500 Internal Server Error
```

**`var error`** — Java type inference (`var` = Java 10+, same as `auto` in C++). Compiler knows the type is `Optional<String>` from `validate()`'s return type.

---

### `WalReplayService.java`

Runs once on startup. Replays WAL to engine before the gateway accepts live orders.

```java
@Component
public class WalReplayService {

    @PostConstruct
    public void replay() {
        // read WAL line by line
        // strip timestamp prefix
        // send each FIX message to engine via sendRaw
    }
}
```

**`@PostConstruct` ordering** — `WalReplayService` injects `EngineClient`. Spring must fully initialise `EngineClient` (including its `@PostConstruct connect()`) before creating `WalReplayService`. By the time `replay()` fires, the TCP connection is already open. No explicit ordering annotation needed — dependency injection order guarantees it.

**Stripping the timestamp:**
```java
int sep = line.indexOf('|');           // find first pipe
String fixMessage = line.substring(sep + 1);  // everything after it
// "1704...|35=D|11=ORD001|..."  →  "35=D|11=ORD001|..."
```

**Execution report responses are discarded** — during replay we only need the engine to rebuild its book state. Trades already happened before the restart and are already in PlanetScale. We don't process the responses.

**Replayed orders are NOT re-written to the WAL** — `sendRaw` goes directly to the engine, bypassing `WalWriter`. Orders already exist in the WAL. Re-writing them would cause double-replay on the next restart.

**`IllegalStateException` if WAL read fails** — if the WAL exists but can't be read, we refuse to start. A gateway that starts with incomplete replay would have a book state mismatch with the engine.

---

## Errors Encountered During Testing

### Error 1 — Gateway started before engine

```
Caused by: java.net.ConnectException: Connection refused: connect
Caused by: java.lang.IllegalStateException: Cannot connect to engine at localhost:7001
```

**Cause:** Gateway started before the engine was running.
**Resolution:** This was the fail-fast behaviour working correctly. Start services in order: engine → consumer → gateway.

---

### Error 2 — Engine blocked waiting for consumer

After starting the engine, it printed:
```
Waiting for consumer on port 7002...
```
And stayed there even after the consumer was started.

**Cause:** The consumer starter we built has no `EventListener` — no code that connects to port 7002. The consumer boots as a plain web server and does nothing else. The engine waits indefinitely for a connection that never comes.

**Resolution:** Used an inline Python heredoc command pasted directly into the terminal to unblock the engine for gateway testing:

```bash
python - <<'EOF'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(('localhost', 7002))
print("Fake consumer connected on port 7002")
while True:
    data = s.recv(4096)
    if not data:
        break
    print("[TRADE]", data.decode(), end='')
EOF
```

The command is stored in `tests/fake_consumer_script.txt` for future reference.

**Why Python and not `nc`:** `nc` (netcat) is not available on this machine. Python's `socket` module provides the same capability and is already installed via Anaconda.

**`python -`** — the `-` tells Python to read the script from stdin rather than a file. `<<'EOF' ... EOF'` is a bash heredoc — everything between the two `EOF` markers is piped as stdin to Python. Single-quoted `'EOF'` prevents bash from expanding `$` variables inside the block.

**Important direction note:** The engine BINDS port 7002 and waits for the consumer to CONNECT to it. The fake consumer connects outward to `localhost:7002`. It does not listen.

---

### Error 3 — `nc` command not found

```bash
nc -lp 7002  # command not found
```

**Cause:** Netcat not installed on this Windows machine.
**Resolution:** Python fake consumer script above.

---

## Test Results

All five tests passed against the live engine + gateway + fake consumer pipeline.

### Test 1 — LIMIT BUY rests in book
```bash
curl -X POST /orders -d '{"orderId":"ORD001","side":"BUY","type":"LIMIT","price":150.00,"quantity":100}'
```
```json
{"orderId":"ORD001","status":"NEW","cumQty":0,"leavesQty":100,"lastPx":null}
```
No asks available → order rested. `status=NEW`, `leavesQty=100`.

---

### Test 2 — Price collar rejection
```bash
curl -X POST /orders -d '{"orderId":"ORD002","side":"BUY","type":"LIMIT","price":200.00,"quantity":100}'
```
```json
{"status":"REJECTED","message":"price 200.00 outside collar [135.00, 165.00]"}
```
$200 is above the $165 upper bound. Rejected before WAL write — ORD002 does not appear in `wal.log`.

---

### Test 3 — Crossing LIMIT SELL → full fill
```bash
curl -X POST /orders -d '{"orderId":"ORD003","side":"SELL","type":"LIMIT","price":150.00,"quantity":100}'
```
```json
{"orderId":"ORD003","status":"FILLED","cumQty":100,"leavesQty":0,"lastPx":150.0}
```
Crossed with ORD001 (BUY @ $150). Both fully filled. Trade event sent to fake consumer.

---

### Test 4 — Place then cancel
```bash
curl -X POST /orders -d '{"orderId":"ORD004","side":"BUY","type":"LIMIT","price":149.00,"quantity":500}'
# → {"status":"NEW","leavesQty":500}

curl -X DELETE /orders/ORD004
# → {"orderId":"ORD004","status":"CANCELLED","cumQty":0,"leavesQty":0}
```
ORD004 rested in book then was cancelled via tombstone.

---

### Test 5 — Duplicate order ID rejection
```bash
curl -X POST /orders -d '{"orderId":"ORD001",...}'
```
```json
{"status":"REJECTED","message":"duplicate orderId: ORD001"}
```
ORD001 was already in the WAL. `WalWriter.contains()` caught it before validation passed.

---

### Test 6 — WAL Replay

Stopped engine and gateway. Restarted in order. Gateway logged:

```
WAL loaded — 3 existing order(s) found.
WalReplayService — replayed 4 order(s). Gateway ready.
```

**Why 3 IDs but 4 lines?**
- `seenIds` = `{ORD001, ORD003, ORD004}` — 3 unique order IDs (ORD002 was rejected, never written)
- WAL has 4 lines: ORD001 new order, ORD003 new order, ORD004 new order, ORD004 cancel

The cancel (`35=F|11=ORD004`) is a separate WAL line but not a new order ID — `ORD004` was already in `seenIds` from the new order line. So 3 IDs, 4 lines. Both numbers are correct.

---

## Spring Boot Concepts Summary

| Concept | Where used | What it does |
|---|---|---|
| `@Component` | All service classes | Registers as a Spring-managed bean |
| `@Autowired` | OrderValidator, OrderController, WalReplayService | Injects a bean created elsewhere |
| `@Value("${key}")` | WalWriter, OrderValidator, EngineClient | Injects value from application.properties |
| `@PostConstruct` | WalWriter, EngineClient, WalReplayService | Runs after bean creation and injection |
| `@PreDestroy` | EngineClient | Runs before bean is destroyed on shutdown |
| `@RestController` | OrderController, HealthController | Marks class as REST controller returning JSON |
| `@RequestMapping` | OrderController | Sets base URL path for all endpoints |
| `@PostMapping` | OrderController | Maps HTTP POST to a method |
| `@DeleteMapping` | OrderController | Maps HTTP DELETE to a method |
| `@RequestBody` | OrderController | Deserialises JSON body into a Java object |
| `@PathVariable` | OrderController | Extracts segment from URL path |
| `ResponseEntity` | OrderController | Controls HTTP status code + body |
| `Optional<T>` | OrderValidator, WalWriter | Null-safe container, forces caller to handle absence |
| `var` | OrderController | Local type inference (Java 10+, same as `auto` in C++) |
| `synchronized` | WalWriter, EngineClient | One thread at a time for concurrent safety |