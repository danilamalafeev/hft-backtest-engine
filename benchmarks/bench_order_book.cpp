#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

#include "lob/order_book.hpp"

namespace {

void SeedBidBook(lob::OrderBook& order_book, std::int64_t count, std::uint64_t starting_id) {
    for (std::int64_t index = 0; index < count; ++index) {
        benchmark::DoNotOptimize(order_book.process_order(lob::Order {
            .id = starting_id + static_cast<std::uint64_t>(index),
            .price = 1'000.0 - static_cast<double>(index),
            .quantity = 10U,
            .side = lob::Side::Buy,
            .timestamp = static_cast<std::uint64_t>(index),
        }));
    }
}

void SeedAskBook(lob::OrderBook& order_book, std::int64_t count, std::uint64_t starting_id) {
    for (std::int64_t index = 0; index < count; ++index) {
        benchmark::DoNotOptimize(order_book.process_order(lob::Order {
            .id = starting_id + static_cast<std::uint64_t>(index),
            .price = 1'000.0 + static_cast<double>(index),
            .quantity = 10U,
            .side = lob::Side::Sell,
            .timestamp = static_cast<std::uint64_t>(index),
        }));
    }
}

void BM_ProcessOrderRestingInsert(benchmark::State& state) {
    const auto initial_book_size = state.range(0);
    std::uint64_t next_order_id = 1'000'000U;

    for (auto _ : state) {
        state.PauseTiming();
        {
            lob::OrderBook order_book {};
            SeedBidBook(order_book, initial_book_size, 1U);
            const auto incoming_order_id = next_order_id++;
            state.ResumeTiming();

            auto trades = order_book.process_order(lob::Order {
                .id = incoming_order_id,
                .price = 2'000.0 + static_cast<double>(incoming_order_id),
                .quantity = 10U,
                .side = lob::Side::Buy,
                .timestamp = incoming_order_id,
            });
            benchmark::DoNotOptimize(trades);
            benchmark::ClobberMemory();
            state.PauseTiming();
        }
        state.ResumeTiming();
    }

    state.SetComplexityN(initial_book_size);
}

void BM_ProcessOrderAggressiveMatch(benchmark::State& state) {
    const auto initial_book_size = state.range(0);
    std::uint64_t next_order_id = 2'000'000U;

    for (auto _ : state) {
        state.PauseTiming();
        {
            lob::OrderBook order_book {};
            SeedAskBook(order_book, initial_book_size, 1U);
            const auto incoming_order_id = next_order_id++;
            state.ResumeTiming();

            auto trades = order_book.process_order(lob::Order {
                .id = incoming_order_id,
                .price = 1'000.0,
                .quantity = 10U,
                .side = lob::Side::Buy,
                .timestamp = incoming_order_id,
            });
            benchmark::DoNotOptimize(trades);
            benchmark::ClobberMemory();
            state.PauseTiming();
        }
        state.ResumeTiming();
    }

    state.SetComplexityN(initial_book_size);
}

void BM_CancelOrder(benchmark::State& state) {
    const auto initial_book_size = state.range(0);
    std::uint64_t next_order_id = 3'000'000U;

    for (auto _ : state) {
        state.PauseTiming();
        {
            lob::OrderBook order_book {};
            SeedBidBook(order_book, initial_book_size, 1U);
            const auto cancel_order_id = next_order_id++;
            benchmark::DoNotOptimize(order_book.process_order(lob::Order {
                .id = cancel_order_id,
                .price = -1'000.0 - static_cast<double>(cancel_order_id),
                .quantity = 10U,
                .side = lob::Side::Buy,
                .timestamp = cancel_order_id,
            }));
            state.ResumeTiming();

            const bool cancelled = order_book.cancel_order(cancel_order_id);
            benchmark::DoNotOptimize(static_cast<int>(cancelled));
            benchmark::ClobberMemory();
            state.PauseTiming();
        }
        state.ResumeTiming();
    }

    state.SetComplexityN(initial_book_size);
}

BENCHMARK(BM_ProcessOrderRestingInsert)
    ->Arg(100)
    ->Arg(1'000)
    ->Arg(10'000)
    ->Complexity();

BENCHMARK(BM_ProcessOrderAggressiveMatch)
    ->Arg(100)
    ->Arg(1'000)
    ->Arg(10'000)
    ->Complexity();

BENCHMARK(BM_CancelOrder)
    ->Arg(100)
    ->Arg(1'000)
    ->Arg(10'000)
    ->Complexity();

}  // namespace
