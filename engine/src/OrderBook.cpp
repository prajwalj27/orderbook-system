#include "OrderBook.h"

void OrderBook::addOrder(const Order& order) {
    if (order.side == Side::BUY) {
        bids[order.price].push_back(order);
        orderIndex[order.orderId] = &bids[order.price].back();
    } else {
        asks[order.price].push_back(order);
        orderIndex[order.orderId] = &asks[order.price].back();
    }
}

bool OrderBook::cancelOrder(const std::string& orderId) {
    auto it = orderIndex.find(orderId);
    if (it == orderIndex.end()) return false;

    it->second->status = OrderStatus::CANCELLED;
    orderIndex.erase(it);
    return true;
}

std::map<double, std::deque<Order>>& OrderBook::getBids() { return bids; }
std::map<double, std::deque<Order>>& OrderBook::getAsks() { return asks; }

bool OrderBook::hasBids() const { return !bids.empty(); }
bool OrderBook::hasAsks() const { return !asks.empty(); }