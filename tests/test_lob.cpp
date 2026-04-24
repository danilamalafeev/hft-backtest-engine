#include <gtest/gtest.h>

#include "lob/order_book.hpp"

namespace {

TEST(OrderBookTest, InitializesSuccessfully) {
    const lob::OrderBook order_book {};
    (void)order_book;

    ASSERT_TRUE(true);
}

TEST(OrderBookTest, ProcessOrderRestsBuyOrderWhenBookDoesNotCross) {
    lob::OrderBook order_book {};
    const lob::Order order {
        .id = 1U,
        .price = 100.25,
        .quantity = 10U,
        .side = lob::Side::Buy,
        .timestamp = 1'000U,
    };

    const auto trades = order_book.process_order(order);

    ASSERT_TRUE(trades.empty());
    ASSERT_EQ(order_book.bids().size(), 1U);
    ASSERT_TRUE(order_book.asks().empty());
    ASSERT_EQ(order_book.order_index().size(), 1U);

    const auto bid_level_it = order_book.bids().find(order.price);
    ASSERT_NE(bid_level_it, order_book.bids().end());
    ASSERT_EQ(bid_level_it->second.orders.size(), 1U);
    EXPECT_EQ(bid_level_it->second.orders.back().id, order.id);
    EXPECT_EQ(order_book.order_index().at(order.id)->id, order.id);
}

TEST(OrderBookTest, ProcessOrderRestsSellOrderWhenBookDoesNotCross) {
    lob::OrderBook order_book {};
    const lob::Order order {
        .id = 2U,
        .price = 100.50,
        .quantity = 12U,
        .side = lob::Side::Sell,
        .timestamp = 1'001U,
    };

    const auto trades = order_book.process_order(order);

    ASSERT_TRUE(trades.empty());
    ASSERT_TRUE(order_book.bids().empty());
    ASSERT_EQ(order_book.asks().size(), 1U);
    ASSERT_EQ(order_book.order_index().size(), 1U);

    const auto ask_level_it = order_book.asks().find(order.price);
    ASSERT_NE(ask_level_it, order_book.asks().end());
    ASSERT_EQ(ask_level_it->second.orders.size(), 1U);
    EXPECT_EQ(ask_level_it->second.orders.back().id, order.id);
    EXPECT_EQ(order_book.order_index().at(order.id)->id, order.id);
}

TEST(OrderBookTest, ProcessOrderMatchesCrossSpreadUsingPriceTimePriority) {
    lob::OrderBook order_book {};

    const auto first_resting_trades = order_book.process_order(lob::Order {
        .id = 10U,
        .price = 100.00,
        .quantity = 5U,
        .side = lob::Side::Sell,
        .timestamp = 1U,
    });
    const auto second_resting_trades = order_book.process_order(lob::Order {
        .id = 11U,
        .price = 100.00,
        .quantity = 7U,
        .side = lob::Side::Sell,
        .timestamp = 2U,
    });
    const auto third_resting_trades = order_book.process_order(lob::Order {
        .id = 12U,
        .price = 101.00,
        .quantity = 4U,
        .side = lob::Side::Sell,
        .timestamp = 3U,
    });

    ASSERT_TRUE(first_resting_trades.empty());
    ASSERT_TRUE(second_resting_trades.empty());
    ASSERT_TRUE(third_resting_trades.empty());

    const auto trades = order_book.process_order(lob::Order {
        .id = 20U,
        .price = 101.00,
        .quantity = 14U,
        .side = lob::Side::Buy,
        .timestamp = 4U,
    });

    ASSERT_EQ(trades.size(), 3U);
    EXPECT_EQ(trades[0].buyer_id, 20U);
    EXPECT_EQ(trades[0].seller_id, 10U);
    EXPECT_DOUBLE_EQ(trades[0].price, 100.00);
    EXPECT_EQ(trades[0].quantity, 5U);

    EXPECT_EQ(trades[1].buyer_id, 20U);
    EXPECT_EQ(trades[1].seller_id, 11U);
    EXPECT_DOUBLE_EQ(trades[1].price, 100.00);
    EXPECT_EQ(trades[1].quantity, 7U);

    EXPECT_EQ(trades[2].buyer_id, 20U);
    EXPECT_EQ(trades[2].seller_id, 12U);
    EXPECT_DOUBLE_EQ(trades[2].price, 101.00);
    EXPECT_EQ(trades[2].quantity, 2U);

    ASSERT_TRUE(order_book.bids().empty());
    ASSERT_EQ(order_book.asks().size(), 1U);
    ASSERT_EQ(order_book.asks().count(100.00), 0U);
    ASSERT_EQ(order_book.order_index().count(10U), 0U);
    ASSERT_EQ(order_book.order_index().count(11U), 0U);
    ASSERT_EQ(order_book.order_index().count(20U), 0U);
    ASSERT_EQ(order_book.order_index().count(12U), 1U);
    ASSERT_EQ(order_book.asks().at(101.00).orders.size(), 1U);
    EXPECT_EQ(order_book.asks().at(101.00).orders.front().id, 12U);
    EXPECT_EQ(order_book.asks().at(101.00).orders.front().quantity, 2U);
}

TEST(OrderBookTest, ProcessOrderRestsResidualQuantityAfterPartialIncomingFill) {
    lob::OrderBook order_book {};

    ASSERT_TRUE(order_book.process_order(lob::Order {
        .id = 30U,
        .price = 100.00,
        .quantity = 4U,
        .side = lob::Side::Sell,
        .timestamp = 10U,
    }).empty());

    const auto trades = order_book.process_order(lob::Order {
        .id = 31U,
        .price = 101.00,
        .quantity = 10U,
        .side = lob::Side::Buy,
        .timestamp = 11U,
    });

    ASSERT_EQ(trades.size(), 1U);
    EXPECT_EQ(trades.front().buyer_id, 31U);
    EXPECT_EQ(trades.front().seller_id, 30U);
    EXPECT_DOUBLE_EQ(trades.front().price, 100.00);
    EXPECT_EQ(trades.front().quantity, 4U);

    ASSERT_TRUE(order_book.asks().empty());
    ASSERT_EQ(order_book.bids().size(), 1U);
    ASSERT_EQ(order_book.order_index().count(30U), 0U);
    ASSERT_EQ(order_book.order_index().count(31U), 1U);
    ASSERT_EQ(order_book.bids().at(101.00).orders.size(), 1U);
    EXPECT_EQ(order_book.bids().at(101.00).orders.front().quantity, 6U);
}

TEST(OrderBookTest, CancelOrderRemovesEmptyBidPriceLevelAndIndexEntry) {
    lob::OrderBook order_book {};
    const lob::Order first_order {
        .id = 10U,
        .price = 99.75,
        .quantity = 15U,
        .side = lob::Side::Buy,
        .timestamp = 2'000U,
    };
    const lob::Order second_order {
        .id = 11U,
        .price = 99.75,
        .quantity = 20U,
        .side = lob::Side::Buy,
        .timestamp = 2'001U,
    };

    ASSERT_TRUE(order_book.process_order(first_order).empty());
    ASSERT_TRUE(order_book.process_order(second_order).empty());

    ASSERT_TRUE(order_book.cancel_order(first_order.id));
    ASSERT_EQ(order_book.bids().size(), 1U);
    ASSERT_EQ(order_book.bids().at(first_order.price).orders.size(), 1U);
    ASSERT_EQ(order_book.order_index().count(first_order.id), 0U);

    ASSERT_TRUE(order_book.cancel_order(second_order.id));
    ASSERT_TRUE(order_book.bids().empty());
    ASSERT_EQ(order_book.order_index().count(second_order.id), 0U);
}

TEST(OrderBookTest, CancelOrderReturnsFalseForUnknownId) {
    lob::OrderBook order_book {};
    EXPECT_FALSE(order_book.cancel_order(999U));
}

}  // namespace
