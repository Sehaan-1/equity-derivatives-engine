#ifndef ORDER_BOOK_HPP
#define ORDER_BOOK_HPP

#include "market_data.hpp"
#include <atomic>
#include <algorithm>
#include <string>
#include <cmath>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace hft {

/**
 * @brief Thread-safe snapshot Limit Order Book (L2 Depth) tracker.
 * Allows single-writer (market feed thread) and multi-reader (strategy threads)
 * to safely read depth, micro-price, and imbalance without data races.
 */
class OrderBook {
public:
    explicit OrderBook(std::string symbol = "") : symbol_(std::move(symbol)) {}

    void update(const Tick& tick) {
        std::unique_lock<std::shared_mutex> lock(tick_mutex_);
        active_tick_ = tick;
        has_data_.store(true, std::memory_order_release);
    }

    Tick get_last_tick() const {
        std::shared_lock<std::shared_mutex> lock(tick_mutex_);
        return active_tick_;
    }

    [[nodiscard]] double best_bid() const {
        Tick t = get_last_tick();
        return static_cast<double>(t.depth.buy[0].price) / 100.0;
    }

    [[nodiscard]] double best_ask() const {
        Tick t = get_last_tick();
        return static_cast<double>(t.depth.sell[0].price) / 100.0;
    }

    [[nodiscard]] double mid_price() const {
        Tick t = get_last_tick();
        double bb = static_cast<double>(t.depth.buy[0].price) / 100.0;
        double ba = static_cast<double>(t.depth.sell[0].price) / 100.0;
        if (bb <= 0.0) return ba;
        if (ba <= 0.0) return bb;
        return (bb + ba) * 0.5;
    }

    [[nodiscard]] double spread() const {
        Tick t = get_last_tick();
        double bb = static_cast<double>(t.depth.buy[0].price) / 100.0;
        double ba = static_cast<double>(t.depth.sell[0].price) / 100.0;
        return ba - bb;
    }

    /**
     * @brief Computes Order Book Imbalance (VOI / Depth ratio):
     * Imbalance = (BidQty - AskQty) / (BidQty + AskQty)
     */
    [[nodiscard]] double get_order_imbalance() const {
        Tick t = get_last_tick();

        uint64_t total_bid_qty = 0;
        uint64_t total_ask_qty = 0;

        for (int i = 0; i < 5; ++i) {
            total_bid_qty += t.depth.buy[i].quantity;
            total_ask_qty += t.depth.sell[i].quantity;
        }

        uint64_t total = total_bid_qty + total_ask_qty;
        if (total == 0) return 0.0;

        return static_cast<double>(static_cast<int64_t>(total_bid_qty) - static_cast<int64_t>(total_ask_qty)) / static_cast<double>(total);
    }

    [[nodiscard]] double micro_price() const {
        Tick t = get_last_tick();
        double bb = static_cast<double>(t.depth.buy[0].price) / 100.0;
        double ba = static_cast<double>(t.depth.sell[0].price) / 100.0;
        double bq = static_cast<double>(t.depth.buy[0].quantity);
        double aq = static_cast<double>(t.depth.sell[0].quantity);

        if (bq + aq <= 0.0) {
            if (bb <= 0.0) return ba;
            if (ba <= 0.0) return bb;
            return (bb + ba) * 0.5;
        }

        return (bb * aq + ba * bq) / (bq + aq);
    }

    [[nodiscard]] bool has_data() const noexcept {
        return has_data_.load(std::memory_order_acquire);
    }

    [[nodiscard]] const std::string& symbol() const noexcept { return symbol_; }

private:
    std::string symbol_;
    alignas(64) std::atomic<bool> has_data_{false};
    mutable std::shared_mutex tick_mutex_;
    Tick active_tick_{};
};

} // namespace hft

#endif // ORDER_BOOK_HPP
