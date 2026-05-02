#pragma once

#include <map>
#include <deque>
#include <unordered_map>
#include <string>
#include "Order.h"

class OrderBook {
public:
    void addOrder(const Order& order);
    bool cancelOrder(const std::string& orderId);

    std::map<double, std::deque<Order>>& getBids();
    std::map<double, std::deque<Order>>& getAsks();

    bool hasBids() const;
    bool hasAsks() const;

private:
    std::map<double, std::deque<Order>> bids;
    std::map<double, std::deque<Order>> asks;
    std::unordered_map<std::string, Order*> orderIndex;
};