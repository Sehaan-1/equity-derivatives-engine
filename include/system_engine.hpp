#ifndef SYSTEM_ENGINE_HPP
#define SYSTEM_ENGINE_HPP

#include "lockfree_ring_buffer.hpp"
#include "arena_allocator.hpp"
#include "market_data.hpp"
#include "order_book.hpp"
#include "risk_engine.hpp"
#include "order_manager.hpp"
#include "strategy.hpp"
#include "latency_tracker.hpp"

#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <iostream>
#include <sstream>
#include <chrono>

namespace hft {

/**
 * @brief Multi-Threaded Execution Engine Orchestrator
 */
class ExecutionEngine {
public:
    ExecutionEngine()
        : running_(false),
          risk_engine_(RiskEngine::RiskLimits{1800, 2500000.0, 3.0, 100, -50000.0}) {}

    ~ExecutionEngine() {
        stop();
    }

    void start() {
        if (running_.load()) return;
        running_.store(true);

        market_data_thread_ = std::thread(&ExecutionEngine::market_data_loop, this);
        strategy_thread_ = std::thread(&ExecutionEngine::strategy_loop, this);
        execution_thread_ = std::thread(&ExecutionEngine::execution_loop, this);
    }

    void stop() {
        if (!running_.load()) return;
        running_.store(false);

        if (market_data_thread_.joinable()) market_data_thread_.join();
        if (strategy_thread_.joinable()) strategy_thread_.join();
        if (execution_thread_.joinable()) execution_thread_.join();
    }

    RiskEngine& get_risk_engine() noexcept { return risk_engine_; }
    OrderManager& get_order_manager() noexcept { return order_manager_; }
    OrderBook& get_order_book() noexcept { return order_book_; }
    LatencyTracker& get_latency_tracker() noexcept { return latency_tracker_; }

    [[nodiscard]] uint64_t get_tick_count() const noexcept { return ticks_processed_.load(); }
    [[nodiscard]] uint64_t get_orders_processed_count() const noexcept { return orders_processed_.load(); }

    [[nodiscard]] bool inject_tick(const Tick& tick) noexcept {
        if (tick_queue_.push(tick)) {
            return true;
        }
        ingress_tick_drops_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    [[nodiscard]] uint64_t get_ingress_tick_drop_count() const noexcept { return ingress_tick_drops_.load(); }
    [[nodiscard]] uint64_t get_strategy_tick_drop_count() const noexcept { return strategy_tick_drops_.load(); }
    [[nodiscard]] uint64_t get_order_drop_count() const noexcept { return order_drops_.load(); }
    [[nodiscard]] uint64_t get_strategy_queue_backpressure_count() const noexcept { return strategy_queue_backpressure_.load(); }
    [[nodiscard]] uint64_t get_order_queue_backpressure_count() const noexcept { return order_queue_backpressure_.load(); }

private:
    void market_data_loop() {
        while (running_.load(std::memory_order_relaxed)) {
            Tick tick;
            if (tick_queue_.pop(tick)) {
                ticks_processed_.fetch_add(1, std::memory_order_relaxed);
                order_book_.update(tick);

                bool pushed = false;
                while (running_.load(std::memory_order_relaxed)) {
                    if (strategy_queue_.push(tick)) {
                        pushed = true;
                        break;
                    }
                    strategy_queue_backpressure_.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::yield();
                }
                if (!pushed) {
                    strategy_tick_drops_.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                std::this_thread::yield();
            }
        }
    }

    void strategy_loop() {
        VWAPImbalanceScalper strategy;
        OrderRequest orders_out[4];

        while (running_.load(std::memory_order_relaxed)) {
            Tick tick;
            if (strategy_queue_.pop(tick)) {
                size_t count = strategy.on_tick(tick, order_book_, orders_out, 4);
                for (size_t i = 0; i < count; ++i) {
                    bool pushed = false;
                    while (running_.load(std::memory_order_relaxed)) {
                        if (order_queue_.push(orders_out[i])) {
                            pushed = true;
                            break;
                        }
                        order_queue_backpressure_.fetch_add(1, std::memory_order_relaxed);
                        std::this_thread::yield();
                    }
                    if (!pushed) {
                        order_drops_.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } else {
                std::this_thread::yield();
            }
        }
    }

    void execution_loop() {
        while (running_.load(std::memory_order_relaxed)) {
            OrderRequest req;
            if (order_queue_.pop(req)) {
                orders_processed_.fetch_add(1, std::memory_order_relaxed);

                uint64_t start_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::high_resolution_clock::now().time_since_epoch()
                    ).count()
                );

                // 1. Instantaneous Pre-trade Risk Check
                RiskCheckResult risk_res = risk_engine_.validate_order(req, order_book_.get_last_tick().ltp);

                uint64_t end_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::high_resolution_clock::now().time_since_epoch()
                    ).count()
                );

                latency_tracker_.record_nanos(end_ns - start_ns);

                if (risk_res == RiskCheckResult::PASSED) {
                    uint64_t oid = order_manager_.create_order(req.symbol, req.instrument_token, req.side, req.type,
                                                               static_cast<double>(req.price_paise) / 100.0, req.quantity);
                    if (oid != 0) {
                        order_manager_.update_status(oid, OrderStatus::NEW);
                        order_manager_.record_fill(oid, static_cast<double>(req.price_paise) / 100.0, req.quantity);
                    } else {
                        order_drops_.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } else {
                std::this_thread::yield();
            }
        }
    }

    std::atomic<bool> running_;
    std::atomic<uint64_t> ticks_processed_{0};
    std::atomic<uint64_t> orders_processed_{0};
    std::atomic<uint64_t> ingress_tick_drops_{0};
    std::atomic<uint64_t> strategy_tick_drops_{0};
    std::atomic<uint64_t> order_drops_{0};
    std::atomic<uint64_t> strategy_queue_backpressure_{0};
    std::atomic<uint64_t> order_queue_backpressure_{0};

    LockFreeSPSCQueue<Tick, 65536> tick_queue_;
    LockFreeSPSCQueue<Tick, 65536> strategy_queue_;
    LockFreeMPSCQueue<OrderRequest, 65536> order_queue_;

    RiskEngine risk_engine_;
    OrderManager order_manager_;
    OrderBook order_book_{"NIFTY26AUGFUT"};
    LatencyTracker latency_tracker_;

    std::thread market_data_thread_;
    std::thread strategy_thread_;
    std::thread execution_thread_;
};

} // namespace hft

#endif // SYSTEM_ENGINE_HPP
