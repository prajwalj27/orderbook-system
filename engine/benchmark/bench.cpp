#include <iostream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <string>
#include <chrono>
#include <random>
#include <map>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

static constexpr int    ENGINE_PORT = 7001;
static constexpr char   ENGINE_HOST[] = "127.0.0.1";
static constexpr int    ORDER_COUNT = 1000000;
static constexpr double REF_PRICE   = 150.0;
static constexpr double SPREAD      = 5.0;

struct OrderMeta {
    std::string id;
    std::string side;   // BUY / SELL
    int         qty;
    double      price;
    std::string fix;
};

static std::vector<OrderMeta> generateOrders(int count) {
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> priceDist(REF_PRICE - SPREAD, REF_PRICE + SPREAD);
    std::uniform_int_distribution<int>     qtyDist(10, 100);
    std::uniform_int_distribution<int>     sideDist(0, 1);

    std::vector<OrderMeta> orders;
    orders.reserve(count);

    for (int i = 1; i <= count; i++) {
        OrderMeta m;
        m.side  = (sideDist(rng) == 0) ? "BUY" : "SELL";
        m.qty   = qtyDist(rng);
        m.price = std::round(priceDist(rng) * 100.0) / 100.0;
        std::ostringstream idss;
        idss << "BN" << std::setfill('0') << std::setw(8) << i;
        m.id = idss.str();

        std::ostringstream ss;
        ss << "35=D"
           << "|11=" << m.id
           << "|54=" << (m.side == "BUY" ? "1" : "2")
           << "|38=" << m.qty
           << "|44=" << std::fixed << std::setprecision(2) << m.price;
        m.fix = ss.str();
        orders.push_back(m);
    }
    return orders;
}

// extract a single tag value from a FIX-style response e.g. "39=2|11=BN000001|14=50"
static std::string getTag(const std::string& line, const std::string& tag) {
    std::string key = tag + "=";
    size_t pos = line.find(key);
    if (pos == std::string::npos) return "";
    pos += key.size();
    size_t end = line.find('|', pos);
    return line.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
}

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(ENGINE_PORT);
    inet_pton(AF_INET, ENGINE_HOST, &addr.sin_addr);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        std::cerr << "Cannot connect to engine on port " << ENGINE_PORT
                  << " - is the engine running?\n";
        WSACleanup();
        return 1;
    }

    auto orders = generateOrders(ORDER_COUNT);

    // ── input summary ─────────────────────────────────────────────────────
    int buyCount = 0, sellCount = 0;
    int totalQty = 0;
    double minPrice = orders[0].price, maxPrice = orders[0].price;

    for (const auto& o : orders) {
        if (o.side == "BUY") buyCount++; else sellCount++;
        totalQty += o.qty;
        if (o.price < minPrice) minPrice = o.price;
        if (o.price > maxPrice) maxPrice = o.price;
    }

    std::cout << "Benchmark Configuration\n";
    std::cout << "-----------------------\n";
    std::cout << "Orders          : " << ORDER_COUNT << "\n";
    std::cout << "BUY / SELL      : " << buyCount << " / " << sellCount << "\n";
    std::cout << "Price range     : " << std::fixed << std::setprecision(2)
              << minPrice << " - " << maxPrice << "\n";
    std::cout << "Qty range       : 10 - 100 (random)\n";
    std::cout << "Total qty       : " << totalQty << "\n";
    std::cout << "Seed            : 42 (reproducible)\n";
    std::cout << "\nConnected to engine on port " << ENGINE_PORT
              << ". Running...\n\n";

    // ── timed section ─────────────────────────────────────────────────────
    auto start = std::chrono::high_resolution_clock::now();

    char        chunk[4096];
    std::string recvBuffer;
    int         responseCount = 0;

    // outcome counters
    int cntNew = 0, cntPartial = 0, cntFilled = 0, cntCancelled = 0;
    int totalCumQty = 0;

    for (const auto& order : orders) {
        std::string line = order.fix + "\n";
        send(sock, line.c_str(), static_cast<int>(line.size()), 0);

        while (recvBuffer.find('\n') == std::string::npos) {
            int bytes = recv(sock, chunk, sizeof(chunk) - 1, 0);
            if (bytes <= 0) goto done;
            chunk[bytes] = '\0';
            recvBuffer += chunk;
        }

        {
            size_t nl  = recvBuffer.find('\n');
            std::string resp = recvBuffer.substr(0, nl);
            recvBuffer.erase(0, nl + 1);
            responseCount++;

            std::string status = getTag(resp, "39");
            int cumQty = 0;
            std::string cqStr = getTag(resp, "14");
            if (!cqStr.empty()) cumQty = std::stoi(cqStr);

            if      (status == "0") cntNew++;
            else if (status == "1") { cntPartial++;  totalCumQty += cumQty; }
            else if (status == "2") { cntFilled++;   totalCumQty += cumQty; }
            else if (status == "4") cntCancelled++;
        }

        // progress every 2000 orders
        if (responseCount % 2000 == 0)
            std::cout << "  " << responseCount << " / " << ORDER_COUNT << " processed...\n";
    }
    done:

    auto end = std::chrono::high_resolution_clock::now();
    // ── end timed section ─────────────────────────────────────────────────

    double elapsedMs  = std::chrono::duration<double, std::milli>(end - start).count();
    double elapsedSec = elapsedMs / 1000.0;
    double throughput = responseCount / elapsedSec;
    double avgLatency = elapsedMs / responseCount;

    std::cout << "\nPerformance\n";
    std::cout << "-----------\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Elapsed         : " << elapsedSec << " s\n";
    std::cout << std::fixed << std::setprecision(0);
    std::cout << "Throughput      : " << throughput << " orders/sec\n";
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "Avg latency     : " << avgLatency << " ms/order\n";

    std::cout << "\nOrder Outcomes\n";
    std::cout << "--------------\n";
    std::cout << "NEW (resting)   : " << cntNew       << "\n";
    std::cout << "PARTIAL         : " << cntPartial   << "\n";
    std::cout << "FILLED          : " << cntFilled    << "\n";
    std::cout << "CANCELLED       : " << cntCancelled << "\n";
    std::cout << "Total qty filled: " << totalCumQty  << "\n";

    closesocket(sock);
    WSACleanup();
    return 0;
}
