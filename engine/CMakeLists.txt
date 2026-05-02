cmake_minimum_required(VERSION 3.20)
project(orderbook VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

add_executable(orderbook
    src/main.cpp
    src/OrderBook.cpp
    src/MatchingEngine.cpp
    src/Parser.cpp
    src/EventLog.cpp
)

target_link_libraries(orderbook ws2_32)
