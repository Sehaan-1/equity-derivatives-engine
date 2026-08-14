#ifndef BACKTEST_ENGINE_HPP
#define BACKTEST_ENGINE_HPP

#include "market_data.hpp"
#include "order_book.hpp"
#include "risk_engine.hpp"
#include "order_manager.hpp"
#include "strategy.hpp"
#include <vector>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <iomanip>

namespace hft {

/**
 * @brief Detailed breakdown of statutory taxes and exchange fees for Indian Equity Derivatives.
 */
struct IndianTaxBreakdown {
    double brokerage{0.0};      // Zerodha ₹20 / order or 0.03%
    double stt{0.0};            // Securities Transaction Tax
    double exchange_fee{0.0};   // NSE Exchange Turnover Fee
    double sebi_fee{0.0};       // SEBI Charges
    double stamp_duty{0.0};     // Stamp Duty
    double gst{0.0};            // 18% GST on (Brokerage + Exchange + SEBI)
    double total_charges{0.0};  // Sum of all friction costs

    void add_trade(OrderSide side, double price, uint32_t qty, bool is_option = false) {
        double turnover = price * qty;

        // 1. Brokerage: Flat ₹20 or 0.03%
        double trade_brokerage = std::min(20.0, turnover * 0.0003);
        brokerage += trade_brokerage;

        // 2. STT (Futures: 0.02% sell side, Options: 0.125% sell side)
        double trade_stt = 0.0;
        if (side == OrderSide::SELL) {
            trade_stt = is_option ? (turnover * 0.00125) : (turnover * 0.0002);
        }
        stt += trade_stt;

        // 3. Exchange Fee (Futures: 0.0019%, Options: 0.05%)
        double trade_exchange = is_option ? (turnover * 0.0005) : (turnover * 0.000019);
        exchange_fee += trade_exchange;

        // 4. SEBI Fee (₹10 / Crore = 0.000001)
        double trade_sebi = turnover * 0.000001;
        sebi_fee += trade_sebi;

        // 5. Stamp Duty (Buy side only: Futures 0.002%, Options 0.003%)
        double trade_stamp = 0.0;
        if (side == OrderSide::BUY) {
            trade_stamp = is_option ? (turnover * 0.00003) : (turnover * 0.00002);
        }
        stamp_duty += trade_stamp;

        // 6. GST (18% on Brokerage + Exchange + SEBI)
        double trade_gst = (trade_brokerage + trade_exchange + trade_sebi) * 0.18;
        gst += trade_gst;

        total_charges = brokerage + stt + exchange_fee + sebi_fee + stamp_duty + gst;
    }
};

struct BacktestResult {
    uint32_t total_ticks_processed{0};
    uint32_t total_orders_submitted{0};
    uint32_t total_orders_executed{0};
    uint32_t total_orders_rejected{0};
    double initial_capital{1000000.0};
    double gross_pnl{0.0};
    double net_pnl{0.0};
    double max_drawdown_pct{0.0};
    double sharpe_ratio{0.0};
    double win_rate_pct{0.0};
    double profit_factor{0.0};
    IndianTaxBreakdown tax_breakdown;
    std::vector<double> equity_curve;
};

/**
 * @brief Event-driven Backtester Simulator
 */
class BacktestEngine {
public:
    BacktestEngine(double initial_capital = 1000000.0)
        : initial_capital_(initial_capital) {}

    BacktestResult run(Strategy& strategy, const std::vector<Tick>& ticks, double slippage_bps = 0.5) {
        OrderBook book;
        RiskEngine risk_engine;
        OrderManager oms;
        IndianTaxBreakdown tax;

        double cash = initial_capital_;
        int32_t position = 0;
        double avg_pos_price = 0.0;
        double peak_equity = initial_capital_;
        double max_drawdown = 0.0;

        uint32_t total_orders = 0;
        uint32_t executed_orders = 0;
        uint32_t rejected_orders = 0;
        uint32_t winning_trades = 0;
        uint32_t total_trades = 0;

        double total_gross_profit = 0.0;
        double total_gross_loss = 0.0;

        std::vector<double> equity_curve;
        equity_curve.reserve(ticks.size());

        OrderRequest strategy_orders[4];

        for (const auto& tick : ticks) {
            book.update(tick);

            size_t count = strategy.on_tick(tick, book, strategy_orders, 4);

            for (size_t i = 0; i < count; ++i) {
                auto req = strategy_orders[i];
                req.client_timestamp_ns = tick.timestamp_ns;
                total_orders++;

                RiskCheckResult risk_res = risk_engine.validate_order_at_ns(req, tick.ltp, tick.timestamp_ns);
                if (risk_res != RiskCheckResult::PASSED) {
                    rejected_orders++;
                    continue;
                }

                uint64_t order_id = oms.create_order(req.symbol, req.instrument_token, req.side, req.type,
                                                      static_cast<double>(req.price_paise) / 100.0, req.quantity);
                if (order_id == 0) {
                    rejected_orders++;
                    continue;
                }

                double base_price = (req.side == OrderSide::BUY) ? book.best_ask() : book.best_bid();
                if (base_price <= 0.0) base_price = tick.get_ltp_double();

                double slippage_mult = (req.side == OrderSide::BUY) ? (1.0 + slippage_bps / 10000.0) : (1.0 - slippage_bps / 10000.0);
                double fill_price = base_price * slippage_mult;

                executed_orders++;
                oms.record_fill(order_id, fill_price, req.quantity);

                bool is_option = std::string(req.symbol).find("CE") != std::string::npos ||
                                 std::string(req.symbol).find("PE") != std::string::npos;
                tax.add_trade(req.side, fill_price, req.quantity, is_option);

                if (req.side == OrderSide::BUY) {
                    if (position < 0) {
                        uint32_t closed_qty = std::min(req.quantity, static_cast<uint32_t>(-position));
                        double trade_pnl = (avg_pos_price - fill_price) * closed_qty;
                        cash += trade_pnl;
                        total_trades++;
                        if (trade_pnl > 0) { winning_trades++; total_gross_profit += trade_pnl; }
                        else { total_gross_loss += std::abs(trade_pnl); }
                    }
                    position += req.quantity;
                    avg_pos_price = fill_price;
                } else { // SELL
                    if (position > 0) {
                        uint32_t closed_qty = std::min(req.quantity, static_cast<uint32_t>(position));
                        double trade_pnl = (fill_price - avg_pos_price) * closed_qty;
                        cash += trade_pnl;
                        total_trades++;
                        if (trade_pnl > 0) { winning_trades++; total_gross_profit += trade_pnl; }
                        else { total_gross_loss += std::abs(trade_pnl); }
                    }
                    position -= req.quantity;
                    avg_pos_price = fill_price;
                }
            }

            double current_ltp = tick.get_ltp_double();
            double unrealized_pnl = position * (current_ltp - avg_pos_price);
            double current_equity = cash + unrealized_pnl - tax.total_charges;

            equity_curve.push_back(current_equity);

            if (current_equity > peak_equity) {
                peak_equity = current_equity;
            } else {
                double dd = (peak_equity - current_equity) / peak_equity * 100.0;
                if (dd > max_drawdown) max_drawdown = dd;
            }
        }

        BacktestResult res{};
        res.total_ticks_processed = static_cast<uint32_t>(ticks.size());
        res.total_orders_submitted = total_orders;
        res.total_orders_executed = executed_orders;
        res.total_orders_rejected = rejected_orders;
        res.initial_capital = initial_capital_;
        res.tax_breakdown = tax;

        double final_equity = equity_curve.empty() ? initial_capital_ : equity_curve.back();
        res.net_pnl = final_equity - initial_capital_;
        res.gross_pnl = res.net_pnl + tax.total_charges;
        res.max_drawdown_pct = max_drawdown;
        res.win_rate_pct = total_trades > 0 ? (static_cast<double>(winning_trades) / total_trades) * 100.0 : 0.0;
        res.profit_factor = total_gross_loss > 0 ? (total_gross_profit / total_gross_loss) : total_gross_profit;

        if (equity_curve.size() > 1) {
            double mean_ret = 0.0;
            std::vector<double> returns;
            returns.reserve(equity_curve.size() - 1);

            for (size_t i = 1; i < equity_curve.size(); ++i) {
                double r = (equity_curve[i] - equity_curve[i - 1]) / equity_curve[i - 1];
                returns.push_back(r);
                mean_ret += r;
            }
            mean_ret /= returns.size();

            double var = 0.0;
            for (double r : returns) {
                var += (r - mean_ret) * (r - mean_ret);
            }
            double stdev = std::sqrt(var / returns.size());
            res.sharpe_ratio = stdev > 0 ? (mean_ret / stdev) * std::sqrt(252.0 * 375.0) : 0.0;
        }

        res.equity_curve = std::move(equity_curve);
        return res;
    }

private:
    double initial_capital_;
};

} // namespace hft

#endif // BACKTEST_ENGINE_HPP
