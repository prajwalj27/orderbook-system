#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include "Order.h"

struct Event {
    OrderStatus type;
    std::string orderId;
    int         cumQty;    // total filled so far (FIX tag 14)
    int         leavesQty; // quantity still open  (FIX tag 151)
    double      lastPx;    // fill price, 0 for ACK/CANCEL (FIX tag 31)
    int64_t     timestamp;
};

class EventLog {
public:
    void record(const Order& order, double lastPx = 0.0);
    std::string formatExecutionReport(const Event& event) const;
    const std::vector<Event>& getEvents() const;

private:
    std::vector<Event> events;
};
