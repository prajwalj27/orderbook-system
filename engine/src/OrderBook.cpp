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

BookSnapshot OrderBook::getSnapshot(int depth) const {
    BookSnapshot snap;

    // bids — iterate from highest price downward
    auto bit = bids.rbegin();
    while (bit != bids.rend() && (int)snap.bids.size() < depth) {
        int totalQty = 0;
        for (const auto& o : bit->second) {
            if (o.status != OrderStatus::CANCELLED)
                totalQty += (o.quantity - o.filledQty);
        }
        if (totalQty > 0)
            snap.bids.push_back({bit->first, totalQty});
        ++bit;
    }

    // asks — iterate from lowest price upward
    auto ait = asks.begin();
    while (ait != asks.end() && (int)snap.asks.size() < depth) {
        int totalQty = 0;
        for (const auto& o : ait->second) {
            if (o.status != OrderStatus::CANCELLED)
                totalQty += (o.quantity - o.filledQty);
        }
        if (totalQty > 0)
            snap.asks.push_back({ait->first, totalQty});
        ++ait;
    }

    return snap;
}
