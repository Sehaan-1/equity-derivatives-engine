#ifndef RISK_ENGINE_HPP
#define RISK_ENGINE_HPP

#include <cstdint>
#include <string>
#include <atomic>
#include <chrono>
#include <array>
#include <cmath>
#include <mutex>

namespace hft {

enum class OrderSide : uint8_t { BUY = 0, SELL = 1 };
enum class OrderType : uint8_t { LIMIT = 0, MARKET = 1, STOP_LOSS = 2 };

struct OrderRequest {
    uint64_t order_id{0};
    uint32_t instrument_token{0};
    char symbol[24]{0};
    OrderSide side{OrderSide::BUY};
    OrderType type{OrderType::LIMIT};
    uint32_t price_paise{0};       // Price in paise (e.g. 2450000 = 24500.00)
    uint32_t quantity{0};          // Number of shares / contracts
    uint64_t client_timestamp_ns{0};
};

enum class RiskCheckResult : uint8_t {
    PASSED = 0,
    REJECTED_MAX_QUANTITY = 1,
    REJECTED_MAX_NOTIONAL = 2,
    REJECTED_PRICE_COLLAR = 3,
    REJECTED_RATE_LIMIT = 4,
    REJECTED_DAILY_DRAWDOWN = 5,
    REJECTED_KILL_SWITCH = 6,
    REJECTED_INVALID_ORDER = 7
};

inline const char* risk_result_to_string(RiskCheckResult result) {
    switch (result) {
        case RiskCheckResult::PASSED: return "PASSED";
        case RiskCheckResult::REJECTED_MAX_QUANTITY: return "REJECT_MAX_QTY";
        case RiskCheckResult::REJECTED_MAX_NOTIONAL: return "REJECT_MAX_NOTIONAL";
        case RiskCheckResult::REJECTED_PRICE_COLLAR: return "REJECT_PRICE_COLLAR";
        case RiskCheckResult::REJECTED_RATE_LIMIT: return "REJECT_RATE_LIMIT";
        case RiskCheckResult::REJECTED_DAILY_DRAWDOWN: return "REJECT_MAX_DRAWDOWN";
        case RiskCheckResult::REJECTED_KILL_SWITCH: return "REJECT_KILL_SWITCH";
        case RiskCheckResult::REJECTED_INVALID_ORDER: return "REJECT_INVALID_ORDER";
        default: return "UNKNOWN_REJECT";
    }
}

/**
 * @brief Deterministic pre-trade risk guardrail engine.
 */
class RiskEngine {
public:
    struct RiskLimits {
        uint32_t max_order_qty = 1800;           // e.g. Max 1800 contracts
        double max_order_notional = 2500000.0;    // e.g. Max ₹25,000,00 (25 Lakhs) per order
        double max_price_collar_pct = 3.0;       // e.g. Max 3% deviation from LTP
        uint32_t max_orders_per_sec = 50;         // Order rate throttle
        double max_daily_drawdown = -50000.0;     // ₹-50,000 maximum allowable daily loss

        RiskLimits() = default;
        RiskLimits(uint32_t qty, double notional, double collar, uint32_t rate, double dd)
            : max_order_qty(qty), max_order_notional(notional), max_price_collar_pct(collar),
              max_orders_per_sec(rate), max_daily_drawdown(dd) {}
    };

    RiskEngine() : RiskEngine(RiskLimits{}) {}
    explicit RiskEngine(const RiskLimits& limits)
        : limits_(limits), kill_switch_active_(false), realized_pnl_(0.0), current_drawdown_(0.0) {
        last_second_time_ = get_current_time_sec();
        order_count_current_sec_ = 0;
    }

    /**
     * @brief Hot-path deterministic risk validation function
     * @param request Incoming order request
     * @param current_ltp_paise Current Last Traded Price in paise
     * @return RiskCheckResult
     */
    RiskCheckResult validate_order(const OrderRequest& request, uint32_t current_ltp_paise) noexcept {
        return validate_order_for_second(request, current_ltp_paise, get_current_time_sec());
    }

    /**
     * @brief Deterministic risk validation using an event timestamp.
     * Backtests should pass the synthetic or historical tick timestamp here so
     * rate limits are evaluated in simulated time instead of wall-clock time.
     */
    RiskCheckResult validate_order_at_ns(const OrderRequest& request, uint32_t current_ltp_paise,
                                         uint64_t timestamp_ns) noexcept {
        return validate_order_for_second(request, current_ltp_paise, timestamp_ns / NS_PER_SECOND);
    }

    RiskCheckResult validate_order_for_second(const OrderRequest& request, uint32_t current_ltp_paise,
                                              uint64_t now_sec) noexcept {
        // 1. Emergency Master Kill Switch check (Atomic load, ~1 ns)
        if (kill_switch_active_.load(std::memory_order_relaxed)) {
            return RiskCheckResult::REJECTED_KILL_SWITCH;
        }

        // 2. Quantity & Basic Validation
        if (request.quantity == 0 || request.quantity > limits_.max_order_qty) {
            return RiskCheckResult::REJECTED_MAX_QUANTITY;
        }

        // 3. Notional Value Check
        double order_price = static_cast<double>(request.price_paise) / 100.0;
        double notional = order_price * request.quantity;
        if (notional > limits_.max_order_notional) {
            return RiskCheckResult::REJECTED_MAX_NOTIONAL;
        }

        // 4. Price Collar / Fat-Finger Prevention Check
        if (current_ltp_paise > 0 && request.type != OrderType::MARKET) {
            double ltp = static_cast<double>(current_ltp_paise) / 100.0;
            double price_diff_pct = (std::abs(order_price - ltp) / ltp) * 100.0;
            if (price_diff_pct > limits_.max_price_collar_pct) {
                return RiskCheckResult::REJECTED_PRICE_COLLAR;
            }
        }

        // 5. Daily Max Drawdown Circuit Breaker
        if (realized_pnl_.load(std::memory_order_relaxed) <= limits_.max_daily_drawdown) {
            return RiskCheckResult::REJECTED_DAILY_DRAWDOWN;
        }

        // 6. Order Throttle / Rate Limiter Check
        uint64_t prev_sec = last_second_time_.load(std::memory_order_relaxed);
        if (now_sec != prev_sec) {
            last_second_time_.store(now_sec, std::memory_order_relaxed);
            order_count_current_sec_.store(1, std::memory_order_relaxed);
        } else {
            uint32_t count = order_count_current_sec_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count > limits_.max_orders_per_sec) {
                return RiskCheckResult::REJECTED_RATE_LIMIT;
            }
        }

        return RiskCheckResult::PASSED;
    }

    void set_kill_switch(bool active) noexcept {
        kill_switch_active_.store(active, std::memory_order_relaxed);
    }

    [[nodiscard]] bool is_kill_switch_active() const noexcept {
        return kill_switch_active_.load(std::memory_order_relaxed);
    }

    void update_pnl(double delta_pnl) noexcept {
        double current = realized_pnl_.load(std::memory_order_relaxed);
        realized_pnl_.store(current + delta_pnl, std::memory_order_relaxed);
    }

    [[nodiscard]] double get_pnl() const noexcept {
        return realized_pnl_.load(std::memory_order_relaxed);
    }

    void set_limits(const RiskLimits& limits) noexcept {
        limits_ = limits;
    }

    [[nodiscard]] const RiskLimits& get_limits() const noexcept {
        return limits_;
    }

private:
    static constexpr uint64_t NS_PER_SECOND = 1000000000ULL;

    static uint64_t get_current_time_sec() noexcept {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
        );
    }

    RiskLimits limits_;
    std::atomic<bool> kill_switch_active_;
    std::atomic<double> realized_pnl_;
    std::atomic<double> current_drawdown_;

    std::atomic<uint64_t> last_second_time_;
    std::atomic<uint32_t> order_count_current_sec_;
};

} // namespace hft

#endif // RISK_ENGINE_HPP
