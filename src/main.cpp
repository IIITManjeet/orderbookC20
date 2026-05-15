#include <cstdio>

#include "order_book/order_book.hpp"

int main() {
    using namespace ob;

    OrderBook book(1024);

    auto print_trade = [](const Trade& t) {
        std::printf("TRADE  maker=%llu taker=%llu  px=%lld  qty=%llu\n",
                    (unsigned long long)t.maker_id,
                    (unsigned long long)t.taker_id,
                    (long long)t.price,
                    (unsigned long long)t.qty);
    };

    book.submit(1, Side::Buy,  OrderType::Limit, 100, 10, 0, print_trade);
    book.submit(2, Side::Buy,  OrderType::Limit,  99, 20, 0, print_trade);
    book.submit(3, Side::Sell, OrderType::Limit, 102, 15, 0, print_trade);
    book.submit(4, Side::Sell, OrderType::Limit, 103,  5, 0, print_trade);

    std::printf("bid=%lld  ask=%lld  resting=%zu\n",
                (long long)book.best_bid(),
                (long long)book.best_ask(),
                book.resting_orders());

    book.submit(5, Side::Buy, OrderType::Limit, 104, 25, 0, print_trade);
    std::printf("bid=%lld  ask=%lld  resting=%zu\n",
                (long long)book.best_bid(),
                (long long)book.best_ask(),
                book.resting_orders());

    book.submit(6, Side::Sell, OrderType::IOC, 100, 50, 0, print_trade);
    std::printf("bid=%lld  ask=%lld  resting=%zu\n",
                (long long)book.best_bid(),
                (long long)book.best_ask(),
                book.resting_orders());

    return 0;
}
