#include <iostream>
#include <string>
#include <sstream>
#include <iomanip>
#include <winsock2.h>
#include <ws2tcpip.h>
#include "OrderBook.h"
#include "MatchingEngine.h"
#include "Parser.h"
#include "EventLog.h"

#pragma comment(lib, "ws2_32.lib")

static SOCKET bindAndListen(int port) {
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<u_short>(port));

    bind(srv, (sockaddr*)&addr, sizeof(addr));
    listen(srv, 1);
    return srv;
}

static void sendLine(SOCKET sock, const std::string& msg) {
    std::string line = msg + "\n";
    send(sock, line.c_str(), static_cast<int>(line.size()), 0);
}

static std::string formatTrade(const Trade& t) {
    std::ostringstream ss;
    ss << "TRADE"
       << "|buyOrderId="  << t.buyOrderId
       << "|sellOrderId=" << t.sellOrderId
       << "|price=" << std::fixed << std::setprecision(2) << t.price
       << "|qty="   << t.quantity
       << "|ts="    << t.timestamp;
    return ss.str();
}

static std::string formatSnapshot(const BookSnapshot& snap) {
    std::ostringstream ss;
    ss << "BOOK";

    ss << "|bids=";
    for (size_t i = 0; i < snap.bids.size(); i++) {
        if (i > 0) ss << ",";
        ss << std::fixed << std::setprecision(2) << snap.bids[i].price
           << "x" << snap.bids[i].quantity;
    }

    ss << "|asks=";
    for (size_t i = 0; i < snap.asks.size(); i++) {
        if (i > 0) ss << ",";
        ss << std::fixed << std::setprecision(2) << snap.asks[i].price
           << "x" << snap.asks[i].quantity;
    }

    return ss.str();
}

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET consumerSrv   = bindAndListen(7002);
    SOCKET marketDataSrv = bindAndListen(7003);
    SOCKET gatewaySrv    = bindAndListen(7001);

    std::cout << "Orderbook Engine v1.0 - listening on 7001 (gateway), 7002 (consumer), 7003 (market-data)" << std::endl;

    std::cout << "Waiting for consumer on port 7002..." << std::endl;
    SOCKET consumerConn = accept(consumerSrv, nullptr, nullptr);
    std::cout << "Consumer connected." << std::endl;

    std::cout << "Waiting for market-data service on port 7003..." << std::endl;
    SOCKET marketDataConn = accept(marketDataSrv, nullptr, nullptr);
    std::cout << "Market-data service connected." << std::endl;

    std::cout << "Waiting for gateway on port 7001..." << std::endl;
    SOCKET gatewayConn = accept(gatewaySrv, nullptr, nullptr);
    std::cout << "Gateway connected. Engine ready." << std::endl;

    OrderBook      book;
    MatchingEngine engine(book);
    EventLog       log;

    std::string buffer;
    char        chunk[4096];

    while (true) {
        int bytes = recv(gatewayConn, chunk, sizeof(chunk) - 1, 0);
        if (bytes <= 0) {
            std::cout << "Gateway disconnected." << std::endl;
            break;
        }
        chunk[bytes] = '\0';
        buffer += chunk;

        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);

            if (line.empty()) continue;

            ParsedMessage msg = Parser::parse(line);

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

            // broadcast book snapshot after every order
            sendLine(marketDataConn, formatSnapshot(book.getSnapshot(5)));
        }
    }

    closesocket(gatewayConn);
    closesocket(consumerConn);
    closesocket(marketDataConn);
    closesocket(gatewaySrv);
    closesocket(consumerSrv);
    closesocket(marketDataSrv);
    WSACleanup();

    return 0;
}
