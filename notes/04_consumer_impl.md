# 04 — Consumer Service Implementation

The consumer is a Spring Boot 3.3.5 service running on port 8081. Its job is to receive trade events from the matching engine over TCP, persist them to a cloud MySQL database, and expose them via a REST API.

---

## Architecture

```
Engine (port 7002) ──TCP──▶ EngineListener ──▶ TradeRepository ──▶ TiDB Cloud (MySQL)
                                                                          ▲
HTTP client ──────────────────────────────────────▶ TradeController ──────┘
```

The engine has two outbound connections: it sends execution reports back to the gateway on port 7001, and sends matched trade events to the consumer on port 7002. These are separate channels with different message formats — the gateway gets FIX-style tag=value responses, the consumer gets a simpler key=value trade format.

---

## Database — TiDB Cloud

**Why TiDB Cloud:** PlanetScale removed its free tier. TiDB Cloud offers a free Serverless tier that is fully MySQL 8.0 compatible. The JDBC connection string, Hibernate dialect, and MySQL driver all work without any changes.

**Connection string format.** TiDB provides a URI in the form:
```
mysql://<user>:<password>@<host>:<port>/<db>
```

Spring Boot needs it split into JDBC format:
```properties
spring.datasource.url=jdbc:mysql://<host>:<port>/<db>?sslMode=VERIFY_IDENTITY
spring.datasource.username=<user>
spring.datasource.password=<password>
spring.datasource.driver-class-name=com.mysql.cj.jdbc.Driver
```

Key differences from the raw URI: `jdbc:` prefix is required, username and password are separate properties, and `?sslMode=VERIFY_IDENTITY` is appended because TiDB Cloud enforces SSL.

**Schema management.** `spring.jpa.hibernate.ddl-auto=update` tells Hibernate to automatically create or alter tables to match the entity definitions on startup. This means you never write `CREATE TABLE` SQL manually — Hibernate generates it. The tradeoff is that `update` only adds columns, it never drops them. If you rename or remove a field from the entity, the old column stays in the database.

---

## Dependencies — pom.xml

Three dependencies were added beyond the base starter:

- `spring-boot-starter-data-jpa` — brings in Hibernate ORM, the JPA API, and Spring Data. This is what lets you write repository interfaces with zero SQL.
- `mysql-connector-j` — the official MySQL JDBC driver. Also works with TiDB Cloud since it is MySQL-compatible.
- `lombok` — same as the gateway, used for `@Data` and `@NoArgsConstructor`.

---

## Trade.java — JPA Entity

```java
@Entity
@Table(name = "trades")
@Data
@NoArgsConstructor
public class Trade {
    @Id
    @GeneratedValue(strategy = GenerationType.IDENTITY)
    private Long id;

    @Column(nullable = false)
    private String buyOrderId;

    @Column(nullable = false)
    private String sellOrderId;

    @Column(nullable = false)
    private double price;

    @Column(nullable = false)
    private int quantity;

    private long engineTimestamp;   // nanosecond epoch from engine

    @Column(nullable = false)
    private Instant receivedAt;
}
```

**What each annotation does:**

- `@Entity` — marks this class as a JPA-managed object that maps to a database table.
- `@Table(name = "trades")` — sets the table name explicitly. Without this, Hibernate would use the class name.
- `@Id` + `@GeneratedValue(IDENTITY)` — the `id` column is the primary key, auto-incremented by the database (`AUTO_INCREMENT` in MySQL).
- `@Column(nullable = false)` — adds a `NOT NULL` constraint at the DB level.
- `Instant receivedAt` — stores the UTC moment the consumer received the message. Hibernate maps `Instant` to `DATETIME(6)` in MySQL.

**Field design:** The entity mirrors the trade message the engine sends — both sides of the trade (buyOrderId, sellOrderId), the matched price, the quantity, and the engine's own timestamp. The `receivedAt` field is added by the consumer to track network/processing latency if needed.

---

## TradeRepository.java — Spring Data Repository

```java
public interface TradeRepository extends JpaRepository<Trade, Long> {
    List<Trade> findByBuyOrderId(String orderId);
    List<Trade> findBySellOrderId(String orderId);

    @Query("SELECT t FROM Trade t WHERE t.buyOrderId = :id OR t.sellOrderId = :id")
    List<Trade> findByOrderId(@Param("id") String orderId);
}
```

`JpaRepository<Trade, Long>` gives you `save()`, `findAll()`, `findById()`, `deleteById()` and more for free — Spring generates the implementation at runtime using reflection.

**Derived query methods:** `findByBuyOrderId` and `findBySellOrderId` are not implemented anywhere. Spring Data reads the method name and automatically generates the SQL (`SELECT * FROM trades WHERE buy_order_id = ?`). This is called a derived query — no SQL needed.

**`@Query`:** The `findByOrderId` method cannot be expressed as a simple derived name (it involves OR across two columns), so it uses JPQL (Java Persistence Query Language). JPQL looks like SQL but operates on entity field names, not column names — `t.buyOrderId` not `buy_order_id`.

---

## EngineListener.java — TCP Client

```java
@PostConstruct
public void start() {
    running = true;
    listenerThread = new Thread(this::listenLoop, "engine-listener");
    listenerThread.setDaemon(true);
    listenerThread.start();
}
```

`@PostConstruct` launches a background thread and returns immediately — Spring startup is not blocked. The thread is a daemon thread, meaning the JVM will not wait for it to finish when the application shuts down.

**`volatile boolean running`** — the flag is read by the listener thread and written by the main thread in `@PreDestroy`. `volatile` guarantees that the write is immediately visible to other threads without needing `synchronized`.

**Retry on disconnect:**
```java
} catch (IOException e) {
    if (!running) break;
    System.err.println("EngineListener lost connection — retrying in 3s");
    Thread.sleep(3000);
}
```

If the engine restarts, the listener reconnects automatically every 3 seconds instead of crashing. This matters for a portfolio project because it means you can restart the engine without restarting the consumer.

**Message format from the engine:**
```
TRADE|buyOrderId=E001|sellOrderId=E002|price=150.00|qty=10|ts=1777758069539721000
```

This is not FIX. The engine sends this custom key=value format to port 7002. The `handleMessage` method filters for lines starting with `TRADE`, parses the key=value pairs, and saves a `Trade` entity.

**`engine.port.consumer:7002`** — the `:7002` in `@Value("${engine.port.consumer:7002}")` is a default value. If the property is missing from `application.properties`, it falls back to 7002. This keeps the gateway config (port 7001) and consumer config (port 7002) cleanly separated.

---

## TradeController.java — REST API

| Endpoint | Response |
|----------|----------|
| `GET /trades` | All rows in the trades table |
| `GET /trades/{orderId}` | All trades where the order ID appears as buyer or seller |

The `/{orderId}` endpoint returns 404 if the order has no trades (e.g. a LIMIT order still sitting in the book). This is intentional — an empty result and a not-found result are different things.

---

## application.properties

```properties
server.port=8081
engine.host=localhost
engine.port.consumer=7002

spring.datasource.url=jdbc:mysql://<host>:4000/<db>?sslMode=VERIFY_IDENTITY
spring.datasource.username=<user>
spring.datasource.password=<password>
spring.datasource.driver-class-name=com.mysql.cj.jdbc.Driver

spring.jpa.database-platform=org.hibernate.dialect.MySQL8Dialect
spring.jpa.hibernate.ddl-auto=update
spring.jpa.show-sql=false
spring.jpa.open-in-view=false
```

`application.properties` is excluded from git via `.gitignore`. The repo contains `application.properties.example` with placeholder values instead. Always rotate credentials if they are accidentally committed.

---

## End-to-End Test

Startup order matters. The engine's `main.cpp` calls `accept()` on port 7002 first, then 7001 — it blocks until both clients connect in that order.

1. Start engine
2. Start consumer (connects to 7002 — engine unblocks)
3. Start gateway (connects to 7001 — engine becomes fully ready)

Test commands:
```bash
# Place a resting buy
curl -s -X POST http://localhost:8080/orders \
  -H "Content-Type: application/json" \
  -d '{"orderId":"E001","side":"BUY","quantity":10,"price":150.00,"type":"LIMIT"}'

# Place a matching sell — crosses the spread, fills E001
curl -s -X POST http://localhost:8080/orders \
  -H "Content-Type: application/json" \
  -d '{"orderId":"E002","side":"SELL","quantity":10,"price":150.00,"type":"LIMIT"}'

# Query all trades persisted by the consumer
curl -s http://localhost:8081/trades | python -m json.tool

# Query trades for a specific order ID
curl -s http://localhost:8081/trades/E001 | python -m json.tool
```

Expected results:
- E001 returns `status: NEW` from the gateway — it entered the book but the response was sent before the match happened (E002 triggers the fill).
- E002 returns `status: FILLED` — the sell matched immediately against E001.
- `GET /trades` returns one row with `buyOrderId: E001`, `sellOrderId: E002`, `price: 150.0`, `quantity: 10`.
- `GET /trades/E001` returns the same row — the query matches on either the buy or sell side.
