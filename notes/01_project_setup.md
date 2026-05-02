# Project Setup — All Four Services

## Project Overview

A production-inspired order book trading system across four services:

```
client/ → gateway/ → engine/ → consumer/ → PlanetScale (MySQL)
```

| Service  | Language       | Port | Role                                          |
|----------|---------------|------|-----------------------------------------------|
| engine   | C++17          | 7001, 7002 | In-memory matching engine              |
| gateway  | Spring Boot    | 8080 | Validates orders, writes WAL, forwards to engine |
| consumer | Spring Boot    | 8081 | Reads fills from engine, persists to PlanetScale |
| client   | Plain Java     | —    | Simulates broker order flow                   |

Communication between services uses plain TCP sockets (not HTTP) except where noted.

---

## Folder Structure

```
orderbook-system/
├── engine/
│   ├── CMakeLists.txt
│   ├── include/
│   ├── src/
│   │   └── main.cpp
│   └── benchmark/
├── gateway/
│   ├── pom.xml
│   └── src/main/java/com/orderbook/gateway/
│       ├── GatewayApplication.java
│       └── HealthController.java
│   └── src/main/resources/
│       └── application.properties
├── consumer/
│   ├── pom.xml
│   └── src/main/java/com/orderbook/consumer/
│       ├── ConsumerApplication.java
│       └── HealthController.java
│   └── src/main/resources/
│       └── application.properties
└── client/
    ├── pom.xml
    └── src/main/java/com/orderbook/client/
        └── ClientSimulator.java
```

---

## Service 1 — C++ Engine

### Directory setup

```bash
mkdir -p engine/src engine/include engine/benchmark
```

### CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.20)
project(orderbook VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

add_executable(orderbook src/main.cpp)
```

Key points:
- `CMAKE_CXX_STANDARD 17` — enforces C++17, hard error if not supported
- `include_directories(include)` — any `#include "Header.h"` looks in `engine/include/`
- `add_executable(orderbook ...)` — the binary will be named `orderbook.exe`

### src/main.cpp (starter)

```cpp
#include <iostream>

int main() {
    std::cout << "Orderbook Engine v1.0 - started" << std::endl;
    return 0;
}
```

### Build commands

```bash
# Configure — run once, or whenever CMakeLists.txt changes
cmake -S . -B build -G "MinGW Makefiles"

# Build — run every time a .cpp or .h file changes
cmake --build build --parallel 8

# Run
./build/orderbook.exe
```

**CMake two-phase mental model:**
- Configure (`cmake -S -B -G`) = set up the kitchen — decides what tools and recipes to use, generates Makefiles in `build/`
- Build (`cmake --build`) = actually cook — compiles only what changed

**Why `-G "MinGW Makefiles"` and not Ninja?**
Ninja is not installed on this machine. MinGW Makefiles uses the `make` tool that ships with GCC 14.2.0 (MinGW-w64).

---

## Service 2 — Spring Boot Gateway (manual setup)

### Directory setup

```bash
mkdir -p gateway/src/main/java/com/orderbook/gateway
mkdir -p gateway/src/main/resources
```

**Why this deep path?** Maven convention requires `src/main/java/` as the source root. The `com/orderbook/gateway` folders must exactly mirror the package declaration in each Java file.

### pom.xml

Maven's build file — equivalent of `CMakeLists.txt` for Java.

Key concepts:
- `<parent>` block — inheriting from `spring-boot-starter-parent` gives you managed versions for all Spring libraries. You never specify Spring dependency versions manually.
- `<artifactId>` — the module name, becomes the jar filename
- `spring-boot-starter-web` — pulls in embedded Tomcat, Spring MVC, Jackson (JSON). One dependency covers everything needed for a web server.
- `spring-boot-devtools` — auto-restarts app when files change during development. `scope=runtime` means it's not packaged into the final jar.
- `spring-boot-maven-plugin` — enables `mvn spring-boot:run` and packages a self-contained fat jar.

### GatewayApplication.java

```java
package com.orderbook.gateway;

import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;

@SpringBootApplication
public class GatewayApplication {
    public static void main(String[] args) {
        SpringApplication.run(GatewayApplication.class, args);
    }
}
```

**Common mistake:** `SpringApplication` (has the `.run()` method) vs `SpringBootApplication` (just the annotation). They look similar but are completely different.

`@SpringBootApplication` bundles three annotations:
- `@Configuration` — this class can define Spring beans
- `@EnableAutoConfiguration` — auto-wire everything based on what's on the classpath
- `@ComponentScan` — scan this package for `@RestController`, `@Service`, etc.

### HealthController.java

```java
package com.orderbook.gateway;

import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.RestController;
import java.util.Map;

@RestController
public class HealthController {

    @GetMapping("/health")
    public Map<String, String> health() {
        return Map.of("status", "UP", "service", "gateway");
    }
}
```

- `@RestController` — every method returns data (JSON), not HTML. Shorthand for `@Controller` + `@ResponseBody`.
- `@GetMapping("/health")` — maps HTTP GET `/health` to this method
- Returning `Map<String, String>` — Jackson automatically serialises it to `{"status":"UP","service":"gateway"}`

### application.properties

```properties
server.port=8080
```

### Run

```bash
cd gateway
mvn spring-boot:run
```

Test:
```bash
curl http://localhost:8080/health
# {"status":"UP","service":"gateway"}
```

---

## Service 3 — Spring Boot Consumer (manual setup)

Identical structure to gateway. Key differences:
- `artifactId` and `name` = `consumer`
- Port = `8081`
- No JPA or MySQL dependency yet — added later when implementing persistence. Adding them now causes Spring Boot to try connecting to PlanetScale on startup and crash.

### Directory setup

```bash
mkdir -p consumer/src/main/java/com/orderbook/consumer
mkdir -p consumer/src/main/resources
```

### application.properties

```properties
server.port=8081
```

### Run

```bash
cd consumer
mvn spring-boot:run
```

Test:
```bash
curl http://localhost:8081/health
# {"status":"UP","service":"consumer"}
```

---

## Service 4 — Java Client (plain Java, no Spring Boot)

### Directory setup

```bash
mkdir -p client/src/main/java/com/orderbook/client
```

### pom.xml differences from Spring Boot services

- No `<parent>` block — not inheriting from Spring Boot
- `maven.compiler.source/target` — must set Java version explicitly (Spring Boot parent does this automatically)
- `maven-jar-plugin` with `<mainClass>` — tells the jar which class to run when you call `java -jar`

### ClientSimulator.java

```java
package com.orderbook.client;

public class ClientSimulator {
    public static void main(String[] args) {
        System.out.println("Orderbook Client Simulator v1.0 - started");
        System.out.println("Gateway target : http://localhost:8080");
        System.out.println("Ready to send orders.");
    }
}
```

### Build and run

```bash
cd client
mvn package -q
java -jar target/client-0.0.1-SNAPSHOT.jar
```

**Why `mvn package` and not `mvn spring-boot:run`?**
There is no Spring Boot plugin in this project. `mvn package` compiles and produces a jar. `java -jar` runs it directly.

---

## Startup Order

This order is mandatory — each service depends on the one before it.

```
1. engine   →  ./build/orderbook.exe
2. consumer →  mvn spring-boot:run   (port 8081, connects to engine port 7002)
3. gateway  →  mvn spring-boot:run   (port 8080, connects to engine port 7001)
4. client   →  java -jar target/client-0.0.1-SNAPSHOT.jar  (optional)
```

---

## Troubleshooting

### `mvn` not found in Git Bash

Git Bash on Windows opens as a login shell and reads `~/.bash_profile`, not `~/.bashrc`. Maven's PATH entry must be in `~/.bash_profile` to persist across VS Code terminals.

```bash
echo 'export PATH="/c/Program Files/Maven/apache-maven-3.9.15/bin:$PATH"' >> ~/.bash_profile
source ~/.bash_profile
```

To keep both files in sync, add this to `~/.bash_profile`:
```bash
if [ -f ~/.bashrc ]; then
    source ~/.bashrc
fi
```

Then you only ever need to edit `~/.bashrc` going forward.

### Spring Initializr (`curl` to `start.spring.io`)

The equivalent of `npx create-react-app` for Spring Boot. Generates the full project structure and `pom.xml` — use it for new services instead of writing `pom.xml` by hand.

```bash
curl "https://start.spring.io/starter.zip?type=maven-project&language=java&bootVersion=3.3.5&baseDir=myservice&groupId=com.orderbook&artifactId=myservice&packageName=com.orderbook.myservice&javaVersion=22&dependencies=web,devtools" -o myservice.zip
unzip myservice.zip
rm myservice.zip
```

### VS Code Java package warning

`"The declared package does not match the expected package"` in VS Code is an IDE indexing issue — it means VS Code hasn't loaded the Maven project correctly and doesn't recognise `src/main/java` as the source root. Install **Extension Pack for Java** (`vscjava.vscode-java-pack`) to fix it. The Maven build (`mvn spring-boot:run`) works fine regardless.
