#include "Parser.h"
#include <sstream>
#include <unordered_map>
#include <chrono>

static std::unordered_map<std::string, std::string> extractTags(const std::string& raw) {
    std::unordered_map<std::string, std::string> tags;
    std::stringstream ss(raw);
    std::string token;

    while (std::getline(ss, token, '|')) {
        auto eq = token.find('=');
        if (eq != std::string::npos) {
            tags[token.substr(0, eq)] = token.substr(eq + 1);
        }
    }

    return tags;
}

ParsedMessage Parser::parse(const std::string& raw) {
    ParsedMessage msg;
    msg.type = MessageType::UNKNOWN;

    auto tags = extractTags(raw);

    if (tags.count("35") == 0) return msg;

    // Cancel message: 35=F|11=ORD001
    if (tags["35"] == "F") {
        msg.type     = MessageType::CANCEL;
        msg.cancelId = tags["11"];
        return msg;
    }

    // New order: 35=D|11=...|54=...|38=...|44=...|40=...
    if (tags["35"] == "D") {
        msg.type = MessageType::NEW_ORDER;

        msg.order.orderId    = tags["11"];
        msg.order.filledQty  = 0;
        msg.order.status     = OrderStatus::NEW;
        msg.order.timestamp  = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::high_resolution_clock::now().time_since_epoch()
                               ).count();

        msg.order.side = (tags["54"] == "1") ? Side::BUY : Side::SELL;

        bool hasPrice = tags.count("44") > 0;
        msg.order.price = hasPrice ? std::stod(tags["44"]) : 0.0;

        if (tags.count("40") > 0) {
            if      (tags["40"] == "1") msg.order.type = OrderType::MARKET;
            else if (tags["40"] == "2") msg.order.type = OrderType::LIMIT;
            else if (tags["40"] == "3") msg.order.type = OrderType::IOC;
        } else {
            msg.order.type = hasPrice ? OrderType::LIMIT : OrderType::MARKET;
        }

        msg.order.quantity = std::stoi(tags["38"]);
        return msg;
    }

    return msg;
}
