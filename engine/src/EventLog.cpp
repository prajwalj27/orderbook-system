#include "EventLog.h"
#include <sstream>
#include <iomanip>

void EventLog::record(const Order& order, double lastPx) {
    int leavesQty = order.quantity - order.filledQty;
    events.push_back({ order.status, order.orderId, order.filledQty, leavesQty, lastPx, order.timestamp });
}

std::string EventLog::formatExecutionReport(const Event& event) const {
    std::ostringstream ss;

    int statusCode = 0;
    switch (event.type) {
        case OrderStatus::NEW:       statusCode = 0; break;
        case OrderStatus::PARTIAL:   statusCode = 1; break;
        case OrderStatus::FILLED:    statusCode = 2; break;
        case OrderStatus::CANCELLED: statusCode = 4; break;
    }

    ss << "39=" << statusCode
       << "|11=" << event.orderId
       << "|14=" << event.cumQty
       << "|151=" << event.leavesQty;

    if (event.type == OrderStatus::PARTIAL || event.type == OrderStatus::FILLED) {
        ss << "|31=" << std::fixed << std::setprecision(2) << event.lastPx;
    }

    return ss.str();
}

const std::vector<Event>& EventLog::getEvents() const {
    return events;
}
