#pragma once

#include <string>
#include <cstdint>

enum class Side {
    BUY,
    SELL
};

enum class OrderType {
    LIMIT,
    MARKET,
    IOC
};

enum class OrderStatus {
    NEW,
    PARTIAL,
    FILLED,
    CANCELLED
};

struct Order {
    std::string  orderId;    // FIX tag 11 — client-assigned unique ID
    Side         side;       // FIX tag 54 — BUY or SELL
    OrderType    type;       // FIX tag 40 — LIMIT, MARKET, or IOC
    double       price;      // FIX tag 44 — omitted for MARKET orders
    int          quantity;   // FIX tag 38 — total shares requested
    int          filledQty;  // shares filled so far, starts at 0
    OrderStatus  status;     // current lifecycle state
    int64_t      timestamp;  // nanoseconds since epoch (std::chrono)
};
