#ifndef STRATEGY_HPP
#define STRATEGY_HPP

#include "market_data.hpp"
#include "order_book.hpp"
#include "risk_engine.hpp"
#include <cmath>
#include <numbers>
#include <array>
#include <string>
#include <string_view>
#include <cstring>

namespace hft {

/**
 * @brief Zero-Allocation Strategy Interface
 */
class Strategy {
public:
    virtual ~Strategy() = default;

    /**
     * @brief Zero-allocation tick callback.
     * @param tick Current tick
     * @param book Order book
     * @param out_orders Output buffer for generated order requests
     * @param max_orders Capacity of output buffer
     * @return Number of orders generated
     */
    virtual size_t on_tick(const Tick& tick, const OrderBook& book, OrderRequest* out_orders, size_t max_orders) = 0;
    virtual const std::string& name() const = 0;
};

/**
 * @brief Indian Black-Scholes Delta Helper Functions
 */
class BlackScholes {
public:
    static double std_norm_cdf(double x) {
        constexpr double INV_SQRT_2 = 0.7071067811865475244008443621048490;
        return 0.5 * std::erfc(-x * INV_SQRT_2);
    }

    static double option_delta(double S, double K, double T_years, double r, double sigma, bool is_call) {
        if (T_years <= 0.0001 || sigma <= 0.0001) {
            if (is_call) return (S > K) ? 1.0 : 0.0;
            else return (S < K) ? -1.0 : 0.0;
        }

        double d1 = (std::log(S / K) + (r + 0.5 * sigma * sigma) * T_years) / (sigma * std::sqrt(T_years));
        return is_call ? std_norm_cdf(d1) : (std_norm_cdf(d1) - 1.0);
    }
};

/**
 * @brief Delta Neutral Gamma Scalping Strategy for NIFTY Options & Futures
 */
class DeltaNeutralGammaScalper : public Strategy {
public:
    DeltaNeutralGammaScalper(double strike = 24500.0, double dte_days = 7.0, double iv = 0.15)
        : strike_(strike), days_to_expiry_(dte_days), implied_volatility_(iv),
          net_delta_(0.0), futures_position_(0), call_position_(50), name_("DeltaNeutralGammaScalper") {}

    size_t on_tick(const Tick& tick, const OrderBook& book, OrderRequest* out_orders, size_t max_orders) override {
        if (!out_orders || max_orders == 0) return 0;

        double spot = book.mid_price();
        if (spot <= 0.0) return 0;

        double t_years = days_to_expiry_ / 365.0;
        double option_delta = BlackScholes::option_delta(spot, strike_, t_years, 0.065, implied_volatility_, true);

        double total_call_delta = call_position_ * option_delta;
        net_delta_ = total_call_delta + futures_position_;

        constexpr double HEDGE_THRESHOLD = 15.0;
        constexpr uint32_t LOT_SIZE = 25; // NIFTY lot size

        if (net_delta_ > HEDGE_THRESHOLD) {
            OrderRequest& req = out_orders[0];
            req = OrderRequest{};
            req.side = OrderSide::SELL;
            req.type = OrderType::MARKET;
            req.quantity = LOT_SIZE;
            req.price_paise = static_cast<uint32_t>(book.best_bid() * 100.0);
            req.instrument_token = tick.instrument_token;
            safe_copy_str(req.symbol, "NIFTY26AUGFUT", sizeof(req.symbol));

            futures_position_ -= LOT_SIZE;
            return 1;
        } else if (net_delta_ < -HEDGE_THRESHOLD) {
            OrderRequest& req = out_orders[0];
            req = OrderRequest{};
            req.side = OrderSide::BUY;
            req.type = OrderType::MARKET;
            req.quantity = LOT_SIZE;
            req.price_paise = static_cast<uint32_t>(book.best_ask() * 100.0);
            req.instrument_token = tick.instrument_token;
            safe_copy_str(req.symbol, "NIFTY26AUGFUT", sizeof(req.symbol));

            futures_position_ += LOT_SIZE;
            return 1;
        }

        return 0;
    }

    const std::string& name() const override { return name_; }
    double net_delta() const noexcept { return net_delta_; }
    int32_t futures_position() const noexcept { return futures_position_; }

private:
    double strike_;
    double days_to_expiry_;
    double implied_volatility_;
    double net_delta_;
    int32_t futures_position_;
    int32_t call_position_;
    std::string name_;
};

/**
 * @brief VWAP Mean Reversion & Micro-Structure Imbalance Scalper
 */
class VWAPImbalanceScalper : public Strategy {
public:
    VWAPImbalanceScalper() : cum_pv_(0.0), cum_v_(0), position_(0), name_("VWAPImbalanceScalper") {}

    size_t on_tick(const Tick& tick, const OrderBook& book, OrderRequest* out_orders, size_t max_orders) override {
        if (!out_orders || max_orders == 0) return 0;

        double price = book.mid_price();
        if (price <= 0.0 || tick.last_quantity == 0) return 0;

        cum_pv_ += price * tick.last_quantity;
        cum_v_ += tick.last_quantity;

        if (cum_v_ == 0) return 0;

        double vwap = cum_pv_ / static_cast<double>(cum_v_);
        double imbalance = book.get_order_imbalance();
        double vwap_diff_pct = ((price - vwap) / vwap) * 100.0;

        constexpr uint32_t LOT_SIZE = 25;

        // Entry Signal
        if (position_ == 0 && vwap_diff_pct < -0.15 && imbalance > 0.3) {
            OrderRequest& req = out_orders[0];
            req = OrderRequest{};
            req.side = OrderSide::BUY;
            req.type = OrderType::LIMIT;
            req.price_paise = static_cast<uint32_t>(book.best_ask() * 100.0);
            req.quantity = LOT_SIZE;
            req.instrument_token = tick.instrument_token;
            safe_copy_str(req.symbol, tick.symbol[0] ? tick.symbol : "NIFTY26AUGFUT", sizeof(req.symbol));

            position_ += LOT_SIZE;
            return 1;
        }
        // Exit Signal
        else if (position_ > 0 && (vwap_diff_pct > 0.10 || imbalance < -0.2)) {
            OrderRequest& req = out_orders[0];
            req = OrderRequest{};
            req.side = OrderSide::SELL;
            req.type = OrderType::LIMIT;
            req.price_paise = static_cast<uint32_t>(book.best_bid() * 100.0);
            req.quantity = static_cast<uint32_t>(position_);
            req.instrument_token = tick.instrument_token;
            safe_copy_str(req.symbol, tick.symbol[0] ? tick.symbol : "NIFTY26AUGFUT", sizeof(req.symbol));

            position_ = 0;
            return 1;
        }

        return 0;
    }

    const std::string& name() const override { return name_; }

private:
    double cum_pv_;
    uint64_t cum_v_;
    int32_t position_;
    std::string name_;
};

} // namespace hft

#endif // STRATEGY_HPP
