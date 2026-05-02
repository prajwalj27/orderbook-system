#include "MatchingEngine.h"
#include <algorithm>
#include <chrono>

MatchingEngine::MatchingEngine(OrderBook& book) : book(book) {}

std::vector<Trade> MatchingEngine::processOrder(Order& order) {
    if (order.side == Side::BUY) return matchBuy(order);
    return matchSell(order);
}

static int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}

std::vector<Trade> MatchingEngine::matchBuy(Order& order) {
    std::vector<Trade> trades;
    auto& asks = book.getAsks();

    while (order.filledQty < order.quantity) {

        if (asks.empty()) break;

        auto bestAskIt = asks.begin();          // lowest ask price
        double bestAskPrice = bestAskIt->first;

        // price crossing check — MARKET orders skip this
        if (order.type != OrderType::MARKET && bestAskPrice > order.price) break;

        auto& levelDeque = bestAskIt->second;

        // skip any tombstoned orders at the front
        while (!levelDeque.empty() && levelDeque.front().status == OrderStatus::CANCELLED) {
            levelDeque.pop_front();
        }
        if (levelDeque.empty()) {
            asks.erase(bestAskIt);
            continue;
        }

        Order& resting = levelDeque.front();
        std::string sellOrderId = resting.orderId;  // save before potential pop_front

        int fillQty = std::min(order.quantity  - order.filledQty,
                               resting.quantity - resting.filledQty);

        order.filledQty   += fillQty;
        resting.filledQty += fillQty;

        order.status   = (order.filledQty   == order.quantity)   ? OrderStatus::FILLED : OrderStatus::PARTIAL;
        resting.status = (resting.filledQty == resting.quantity) ? OrderStatus::FILLED : OrderStatus::PARTIAL;

        if (resting.status == OrderStatus::FILLED) {
            levelDeque.pop_front();
            if (levelDeque.empty()) asks.erase(bestAskIt);
        }

        trades.push_back({ order.orderId, sellOrderId, bestAskPrice, fillQty, now_ns() });
    }

    // handle remainder
    if (order.filledQty < order.quantity) {
        if (order.type == OrderType::LIMIT) {
            order.status = (order.filledQty > 0) ? OrderStatus::PARTIAL : OrderStatus::NEW;
            book.addOrder(order);
        } else {
            order.status = OrderStatus::CANCELLED;  // MARKET or IOC — never rest
        }
    }

    return trades;
}

std::vector<Trade> MatchingEngine::matchSell(Order& order) {
    std::vector<Trade> trades;
    auto& bids = book.getBids();

    while (order.filledQty < order.quantity) {

        if (bids.empty()) break;

        auto bestBidIt = std::prev(bids.end());  // highest bid price
        double bestBidPrice = bestBidIt->first;

        // price crossing check — MARKET orders skip this
        if (order.type != OrderType::MARKET && bestBidPrice < order.price) break;

        auto& levelDeque = bestBidIt->second;

        // skip tombstoned orders
        while (!levelDeque.empty() && levelDeque.front().status == OrderStatus::CANCELLED) {
            levelDeque.pop_front();
        }
        if (levelDeque.empty()) {
            bids.erase(bestBidIt);
            continue;
        }

        Order& resting = levelDeque.front();
        std::string buyOrderId = resting.orderId;

        int fillQty = std::min(order.quantity  - order.filledQty,
                               resting.quantity - resting.filledQty);

        order.filledQty   += fillQty;
        resting.filledQty += fillQty;

        order.status   = (order.filledQty   == order.quantity)   ? OrderStatus::FILLED : OrderStatus::PARTIAL;
        resting.status = (resting.filledQty == resting.quantity) ? OrderStatus::FILLED : OrderStatus::PARTIAL;

        if (resting.status == OrderStatus::FILLED) {
            levelDeque.pop_front();
            if (levelDeque.empty()) bids.erase(bestBidIt);
        }

        trades.push_back({ buyOrderId, order.orderId, bestBidPrice, fillQty, now_ns() });
    }

    // handle remainder
    if (order.filledQty < order.quantity) {
        if (order.type == OrderType::LIMIT) {
            order.status = (order.filledQty > 0) ? OrderStatus::PARTIAL : OrderStatus::NEW;
            book.addOrder(order);
        } else {
            order.status = OrderStatus::CANCELLED;
        }
    }

    return trades;
}
