#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include "Order.h"
#include "OrderBook.h"

struct Trade {
    std::string buyOrderId;
    std::string sellOrderId;
    double      price;
    int         quantity;
    int64_t     timestamp;
};

class MatchingEngine {
public:
    explicit MatchingEngine(OrderBook& book);

    std::vector<Trade> processOrder(Order& order);

private:
    OrderBook& book;

    std::vector<Trade> matchBuy(Order& order);
    std::vector<Trade> matchSell(Order& order);
};
