#pragma once

#include <map>
#include <deque>
#include <unordered_map>
#include <string>
#include <vector>
#include "Order.h"

struct PriceLevel {
    double price;
    int    quantity;   // total remaining qty at this level (excluding filled/cancelled)
};

struct BookSnapshot {
    std::vector<PriceLevel> bids;  // best (highest) first
    std::vector<PriceLevel> asks;  // best (lowest) first
};

class OrderBook {
public:
    void addOrder(const Order& order);
    bool cancelOrder(const std::string& orderId);

    std::map<double, std::deque<Order>>& getBids();
    std::map<double, std::deque<Order>>& getAsks();

    bool         hasBids() const;
    bool         hasAsks() const;
    BookSnapshot getSnapshot(int depth = 5) const;

private:
    std::map<double, std::deque<Order>> bids;
    std::map<double, std::deque<Order>> asks;
    std::unordered_map<std::string, Order*> orderIndex;
};
