#include <gtest/gtest.h>

#include "lob/order_book.hpp"

namespace {

TEST(OrderBookTest, InitializesSuccessfully) {
    const lob::OrderBook order_book {};
    (void)order_book;

    ASSERT_TRUE(true);
}

}  // namespace
