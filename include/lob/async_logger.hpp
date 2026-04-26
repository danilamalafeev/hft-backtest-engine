#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include "lob/event.hpp"

namespace lob {

enum class LogEventType : std::uint8_t {
    MarketEvent = 0U,
    Fill = 1U,
    EquitySample = 2U,
    Drop = 3U,
    OrderGroup = 4U,
    PanicClose = 5U,
    PartialFill = 6U
};

struct LogRecord {
    std::uint64_t nanoseconds {};
    AssetID asset_id {};
    LogEventType event_type {LogEventType::MarketEvent};
    std::uint64_t group_id {};
    Side side {Side::Buy};
    double price {};
    double qty {};
    double pnl_impact {};
    double current_nav {};
};

template <std::size_t Capacity = 1U << 20U>
class AsyncLogger {
    static_assert(Capacity > 1U, "AsyncLogger requires capacity > 1");

public:
    AsyncLogger() = default;

    explicit AsyncLogger(const std::string& path) {
        open(path);
    }

    ~AsyncLogger() {
        stop();
    }

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;
    AsyncLogger(AsyncLogger&&) = delete;
    AsyncLogger& operator=(AsyncLogger&&) = delete;

    void open(const std::string& path) {
        output_.open(path, std::ios::out | std::ios::trunc);
        output_ << "nanoseconds,asset_id,event_type,group_id,side,price,qty,pnl_impact,current_nav\n";
        running_.store(true, std::memory_order_release);
        worker_ = std::thread(&AsyncLogger::consume_loop, this);
    }

    void stop() noexcept {
        const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
        if (!was_running) {
            return;
        }

        cv_.notify_one();
        if (worker_.joinable()) {
            worker_.join();
        }

        flush_available();
        output_.flush();
        output_.close();
    }

    [[nodiscard]] bool push(const LogRecord& record) noexcept {
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        const std::uint64_t next_tail = tail + 1U;
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        if (next_tail - head > Capacity) [[unlikely]] {
            dropped_.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }

        buffer_[tail % Capacity] = record;
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    void consume_loop() {
        pin_worker_thread_best_effort();

        while (running_.load(std::memory_order_acquire) || has_available()) {
            if (!flush_available()) {
                std::unique_lock<std::mutex> lock {mutex_};
                cv_.wait_for(lock, std::chrono::milliseconds(1), [this] {
                    return !running_.load(std::memory_order_acquire) || has_available();
                });
            }
        }
    }

    static void pin_worker_thread_best_effort() noexcept {
#if defined(__linux__)
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(1, &cpuset);
        (void)pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
    }

    [[nodiscard]] bool has_available() const noexcept {
        return head_.load(std::memory_order_acquire) != tail_.load(std::memory_order_acquire);
    }

    bool flush_available() {
        bool wrote_any = false;
        std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);

        while (head != tail) {
            const LogRecord& record = buffer_[head % Capacity];
            output_ << record.nanoseconds << ','
                    << record.asset_id << ','
                    << static_cast<unsigned>(record.event_type) << ','
                    << record.group_id << ','
                    << (record.side == Side::Buy ? "BUY" : "SELL") << ','
                    << record.price << ','
                    << record.qty << ','
                    << record.pnl_impact << ','
                    << record.current_nav << '\n';
            ++head;
            wrote_any = true;
        }

        if (wrote_any) {
            head_.store(head, std::memory_order_release);
        }
        return wrote_any;
    }

    alignas(64) std::array<LogRecord, Capacity> buffer_ {};
    alignas(64) std::atomic<std::uint64_t> head_ {};
    alignas(64) std::atomic<std::uint64_t> tail_ {};
    std::atomic<std::uint64_t> dropped_ {};
    std::atomic<bool> running_ {};
    std::ofstream output_ {};
    std::thread worker_ {};
    std::mutex mutex_ {};
    std::condition_variable cv_ {};
};

}  // namespace lob
