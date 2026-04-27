#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include "lob/analytics.hpp"
#include "lob/csv_parser.hpp"
#include "lob/dynamic_wallet.hpp"
#include "lob/event_merger.hpp"
#include "lob/l2_depth5_csv_parser.hpp"
#include "lob/multi_asset_backtest_engine.hpp"
#include "lob/order_book.hpp"

namespace {

template <std::size_t Capacity>
class FakeStreamParser {
public:
    FakeStreamParser() = default;

    explicit FakeStreamParser(std::array<std::uint64_t, Capacity> timestamps, std::size_t size)
        : timestamps_(timestamps),
          size_(size) {}

    [[nodiscard]] bool has_next() const noexcept {
        return index_ < size_;
    }

    [[nodiscard]] std::uint64_t peek_time() const noexcept {
        return timestamps_[index_];
    }

    [[nodiscard]] lob::Event pop() noexcept {
        const std::uint64_t timestamp = timestamps_[index_++];
        return lob::Event {
            .timestamp = timestamp,
            .order = lob::Order {
                .id = timestamp,
                .price = 100.0,
                .quantity = 0U,
                .side = lob::Side::Buy,
                .timestamp = timestamp,
            },
        };
    }

private:
    std::array<std::uint64_t, Capacity> timestamps_ {};
    std::size_t size_ {};
    std::size_t index_ {};
};

class CountingStrategy final : public lob::Strategy {
public:
    void on_tick(lob::AssetID asset_id, const lob::OrderBook& book, lob::OrderGateway& gateway) override {
        (void)book;
        (void)gateway;
        ++ticks_by_asset_[asset_id];
    }

    std::array<std::uint64_t, 2U> ticks_by_asset_ {};
};

TEST(OrderBookTest, InitializesSuccessfully) {
    const lob::OrderBook order_book {};
    (void)order_book;

    ASSERT_TRUE(true);
}

TEST(DynamicWalletTest, TracksReservedAndFreeBalances) {
    lob::DynamicWallet wallet {};
    wallet.reset(2U, 0U, 100.0);

    EXPECT_TRUE(wallet.reserve_balance(0U, 60.0));
    EXPECT_DOUBLE_EQ(wallet.balance(0U), 100.0);
    EXPECT_DOUBLE_EQ(wallet.reserved(0U), 60.0);
    EXPECT_DOUBLE_EQ(wallet.free_balance(0U), 40.0);
    EXPECT_FALSE(wallet.reserve_balance(0U, 50.0));

    EXPECT_TRUE(wallet.consume_reserved(0U, 25.0));
    EXPECT_DOUBLE_EQ(wallet.balance(0U), 75.0);
    EXPECT_DOUBLE_EQ(wallet.reserved(0U), 35.0);
    EXPECT_DOUBLE_EQ(wallet.free_balance(0U), 40.0);

    wallet.release_reserved(0U, 35.0);
    EXPECT_DOUBLE_EQ(wallet.balance(0U), 75.0);
    EXPECT_DOUBLE_EQ(wallet.reserved(0U), 0.0);
    EXPECT_DOUBLE_EQ(wallet.free_balance(0U), 75.0);
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
    EXPECT_EQ(order_book.order_index().at(order.id).order_iterator->id, order.id);
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
    EXPECT_EQ(order_book.order_index().at(order.id).order_iterator->id, order.id);
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
    EXPECT_EQ(trades[0].taker_order_id, 20U);
    EXPECT_DOUBLE_EQ(trades[0].price, 100.00);
    EXPECT_EQ(trades[0].quantity, 5U);

    EXPECT_EQ(trades[1].buyer_id, 20U);
    EXPECT_EQ(trades[1].seller_id, 11U);
    EXPECT_EQ(trades[1].taker_order_id, 20U);
    EXPECT_DOUBLE_EQ(trades[1].price, 100.00);
    EXPECT_EQ(trades[1].quantity, 7U);

    EXPECT_EQ(trades[2].buyer_id, 20U);
    EXPECT_EQ(trades[2].seller_id, 12U);
    EXPECT_EQ(trades[2].taker_order_id, 20U);
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

TEST(OrderBookTest, ExposesBestPricesAndL2Snapshot) {
    lob::OrderBook order_book {};
    ASSERT_TRUE(order_book.process_order(lob::Order {
        .id = 100U,
        .price = 99.50,
        .quantity = 10U,
        .side = lob::Side::Buy,
        .timestamp = 1U,
    }).empty());
    ASSERT_TRUE(order_book.process_order(lob::Order {
        .id = 101U,
        .price = 99.50,
        .quantity = 15U,
        .side = lob::Side::Buy,
        .timestamp = 2U,
    }).empty());
    ASSERT_TRUE(order_book.process_order(lob::Order {
        .id = 102U,
        .price = 100.50,
        .quantity = 20U,
        .side = lob::Side::Sell,
        .timestamp = 3U,
    }).empty());

    EXPECT_DOUBLE_EQ(order_book.get_best_bid(), 99.50);
    EXPECT_DOUBLE_EQ(order_book.get_best_ask(), 100.50);

    std::vector<lob::PriceLevelInfo> bids {};
    std::vector<lob::PriceLevelInfo> asks {};
    bids.reserve(5U);
    asks.reserve(5U);

    order_book.get_l2_snapshot(bids, asks, 5);

    ASSERT_EQ(bids.size(), 1U);
    ASSERT_EQ(asks.size(), 1U);
    EXPECT_DOUBLE_EQ(bids.front().price, 99.50);
    EXPECT_DOUBLE_EQ(bids.front().total_qty, 25.0);
    EXPECT_DOUBLE_EQ(asks.front().price, 100.50);
    EXPECT_DOUBLE_EQ(asks.front().total_qty, 20.0);
}

TEST(AnalyticsTest, ComputesPnlAndDrawdownFromEquityCurve) {
    const std::vector<double> equity_curve {100.0, 110.0, 105.0, 120.0};

    const lob::BacktestAnalytics analytics = lob::BacktestAnalytics::analyze(equity_curve);

    EXPECT_DOUBLE_EQ(analytics.total_realized_pnl, 20.0);
    EXPECT_DOUBLE_EQ(analytics.max_drawdown, 5.0);
    EXPECT_DOUBLE_EQ(analytics.max_drawdown_pct, 5.0 / 110.0);
    EXPECT_GT(analytics.sharpe_ratio, 0.0);
}

TEST(CsvParserTest, StreamsMmapEventsWithOneEventLookahead) {
    const std::filesystem::path csv_path = std::filesystem::temp_directory_path() / "lob_streaming_parser_test.csv";
    {
        std::ofstream csv {csv_path};
        csv << "1,100.00,0.01000000,1.00000000,1709251200000,false,true\n"
            << "2,101.25,0.02000000,2.02500000,1709251200100,true,true\n";
    }

    lob::CsvParser parser {csv_path};

    ASSERT_TRUE(parser.has_next());
    EXPECT_EQ(parser.peek_time(), 1'709'251'200'000ULL);

    const lob::Event first_event = parser.pop();
    EXPECT_EQ(first_event.timestamp, 1'709'251'200'000ULL);
    EXPECT_EQ(first_event.order.id, 1U);
    EXPECT_EQ(first_event.order.side, lob::Side::Buy);
    EXPECT_EQ(first_event.order.quantity, 1'000'000U);

    ASSERT_TRUE(parser.has_next());
    EXPECT_EQ(parser.peek_time(), 1'709'251'200'100ULL);

    const lob::Event second_event = parser.pop();
    EXPECT_EQ(second_event.order.id, 2U);
    EXPECT_EQ(second_event.order.side, lob::Side::Sell);
    EXPECT_EQ(second_event.order.quantity, 2'000'000U);
    EXPECT_FALSE(parser.has_next());

    std::filesystem::remove(csv_path);
}

TEST(L2Depth5CsvParserTest, StreamsDepth5RowsAndBackfillsBboRows) {
    const std::filesystem::path depth_path = std::filesystem::temp_directory_path() / "lob_depth5_parser_test.csv";
    {
        std::ofstream csv {depth_path};
        csv << "timestamp,b1_p,b1_q,b2_p,b2_q,b3_p,b3_q,b4_p,b4_q,b5_p,b5_q,"
               "a1_p,a1_q,a2_p,a2_q,a3_p,a3_q,a4_p,a4_q,a5_p,a5_q\n"
            << "1000,99,1,98,2,97,3,96,4,95,5,101,6,102,7,103,8,104,9,105,10\n";
    }

    lob::L2Depth5CsvParser depth_parser {depth_path};
    ASSERT_TRUE(depth_parser.has_next());
    const lob::L2Depth5Event& depth_event = depth_parser.peek();
    EXPECT_EQ(depth_event.timestamp, 1000U);
    EXPECT_DOUBLE_EQ(depth_event.bid_prices[0], 99.0);
    EXPECT_DOUBLE_EQ(depth_event.bid_qty[4], 5.0);
    EXPECT_DOUBLE_EQ(depth_event.ask_prices[0], 101.0);
    EXPECT_DOUBLE_EQ(depth_event.ask_qty[4], 10.0);
    depth_parser.advance();
    EXPECT_FALSE(depth_parser.has_next());
    std::filesystem::remove(depth_path);

    const std::filesystem::path bbo_path = std::filesystem::temp_directory_path() / "lob_depth5_bbo_parser_test.csv";
    {
        std::ofstream csv {bbo_path};
        csv << "timestamp,bid_price,bid_qty,ask_price,ask_qty\n"
            << "2000,100,2,101,3\n";
    }

    lob::L2Depth5CsvParser bbo_parser {bbo_path};
    ASSERT_TRUE(bbo_parser.has_next());
    const lob::L2Depth5Event& bbo_event = bbo_parser.peek();
    EXPECT_EQ(bbo_event.timestamp, 2000U);
    EXPECT_DOUBLE_EQ(bbo_event.bid_prices[0], 100.0);
    EXPECT_DOUBLE_EQ(bbo_event.bid_qty[0], 2.0);
    EXPECT_DOUBLE_EQ(bbo_event.ask_prices[0], 101.0);
    EXPECT_DOUBLE_EQ(bbo_event.ask_qty[0], 3.0);
    EXPECT_DOUBLE_EQ(bbo_event.bid_prices[1], 0.0);
    EXPECT_DOUBLE_EQ(bbo_event.ask_qty[1], 0.0);
    bbo_parser.advance();
    EXPECT_FALSE(bbo_parser.has_next());
    std::filesystem::remove(bbo_path);
}

TEST(EventMergerTest, MergesSmallStreamSetWithFastPath) {
    using Parser = FakeStreamParser<3U>;
    lob::EventMerger<3U, Parser> merger {std::array<Parser, 3U> {
        Parser {{1U, 4U, 7U}, 3U},
        Parser {{2U, 5U, 8U}, 3U},
        Parser {{3U, 6U, 9U}, 3U},
    }};

    for (std::uint64_t expected_timestamp = 1U; expected_timestamp <= 9U; ++expected_timestamp) {
        ASSERT_TRUE(merger.has_next());
        const lob::Event event = merger.get_next();
        EXPECT_EQ(event.timestamp, expected_timestamp);
        EXPECT_EQ(event.asset_id, static_cast<lob::AssetID>((expected_timestamp - 1U) % 3U));
    }

    EXPECT_FALSE(merger.has_next());
}

TEST(EventMergerTest, MergesLargeStreamSetWithHeapPath) {
    using Parser = FakeStreamParser<2U>;
    lob::EventMerger<5U, Parser> merger {std::array<Parser, 5U> {
        Parser {{5U, 10U}, 2U},
        Parser {{1U, 6U}, 2U},
        Parser {{3U, 8U}, 2U},
        Parser {{2U, 7U}, 2U},
        Parser {{4U, 9U}, 2U},
    }};

    for (std::uint64_t expected_timestamp = 1U; expected_timestamp <= 10U; ++expected_timestamp) {
        ASSERT_TRUE(merger.has_next());
        const lob::Event event = merger.get_next();
        EXPECT_EQ(event.timestamp, expected_timestamp);
    }

    EXPECT_FALSE(merger.has_next());
}

TEST(MultiAssetBacktestEngineTest, RoutesMergedEventsToAssetBooksAndStrategy) {
    using Parser = FakeStreamParser<2U>;
    CountingStrategy strategy {};
    lob::MultiAssetBacktestEngine<2U, Parser> engine {strategy, std::array<Parser, 2U> {
        Parser {{1U, 3U}, 2U},
        Parser {{2U, 4U}, 2U},
    }};

    const auto result = engine.run();

    EXPECT_EQ(result.events_processed, 4U);
    EXPECT_EQ(strategy.ticks_by_asset_[0], 2U);
    EXPECT_EQ(strategy.ticks_by_asset_[1], 2U);
}

}  // namespace
