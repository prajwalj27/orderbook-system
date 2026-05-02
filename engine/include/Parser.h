#pragma once

#include <string>
#include "Order.h"

enum class MessageType {
    NEW_ORDER,
    CANCEL,
    UNKNOWN
};

struct ParsedMessage {
    MessageType type;
    Order       order;      // valid when type == NEW_ORDER
    std::string cancelId;   // valid when type == CANCEL
};

class Parser {
public:
    static ParsedMessage parse(const std::string& raw);
};
