#ifndef ORDER_MANAGER_HPP
#define ORDER_MANAGER_HPP

#include "market_data.hpp"
#include "risk_engine.hpp"
#include "arena_allocator.hpp"
#include <array>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <span>
#include <sstream>
#include <iomanip>
#include <cstring>

namespace hft {

enum class OrderStatus : uint8_t {
    PENDING_NEW = 0,
    NEW = 1,
    PARTIALLY_FILLED = 2,
    FILLED = 3,
    CANCELLED = 4,
    REJECTED = 5
};

inline const char* order_status_to_string(OrderStatus status) {
    switch (status) {
        case OrderStatus::PENDING_NEW: return "PENDING_NEW";
        case OrderStatus::NEW: return "OPEN";
        case OrderStatus::PARTIALLY_FILLED: return "PARTIAL";
        case OrderStatus::FILLED: return "COMPLETE";
        case OrderStatus::CANCELLED: return "CANCELLED";
        case OrderStatus::REJECTED: return "REJECTED";
        default: return "UNKNOWN";
    }
}

struct ExecutionReport {
    uint64_t order_id{0};
    uint64_t exec_id{0};
    uint32_t instrument_token{0};
    char symbol[24]{0};
    OrderSide side{OrderSide::BUY};
    OrderStatus status{OrderStatus::NEW};
    uint32_t fill_price_paise{0};
    uint32_t fill_quantity{0};
    uint32_t cum_quantity{0};
    uint32_t leaves_quantity{0};
    uint64_t timestamp_ns{0};
    RiskCheckResult risk_result{RiskCheckResult::PASSED};
};

struct ActiveOrder {
    OrderRequest request;
    OrderStatus status{OrderStatus::PENDING_NEW};
    uint32_t filled_quantity{0};
    uint32_t avg_fill_price_paise{0};
    uint64_t created_time_ns{0};
    uint64_t last_update_ns{0};
};

/**
 * @brief Order Management System (OMS) State Engine.
 * Uses ObjectPool pre-allocated arena memory pools for zero-allocation hot paths.
 */
class OrderManager {
public:
    OrderManager() : next_order_id_(100001), next_exec_id_(500001) {
        active_orders_.fill(nullptr);
        execution_history_.fill(nullptr);
    }

    uint64_t create_order(std::string_view symbol, uint32_t instrument_token, OrderSide side,
                         OrderType type, double price, uint32_t qty) {
        if (active_order_count_ >= MAX_ACTIVE_ORDERS) {
            return 0;
        }

        uint64_t id = next_order_id_++;
        OrderRequest req{};
        req.order_id = id;
        req.instrument_token = instrument_token;
        safe_copy_str(req.symbol, symbol, sizeof(req.symbol));
        req.side = side;
        req.type = type;
        req.price_paise = static_cast<uint32_t>(price * 100.0 + 0.5);
        req.quantity = qty;
        req.client_timestamp_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now().time_since_epoch()
            ).count()
        );

        ActiveOrder* ord = order_pool_.allocate();
        ord->request = req;
        ord->status = OrderStatus::PENDING_NEW;
        ord->created_time_ns = req.client_timestamp_ns;
        ord->last_update_ns = req.client_timestamp_ns;
        ord->filled_quantity = 0;
        ord->avg_fill_price_paise = 0;

        active_orders_[active_order_count_++] = ord;
        return id;
    }

    bool update_status(uint64_t order_id, OrderStatus new_status) {
        for (size_t i = 0; i < active_order_count_; ++i) {
            auto* ord = active_orders_[i];
            if (ord && ord->request.order_id == order_id) {
                ord->status = new_status;
                ord->last_update_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::high_resolution_clock::now().time_since_epoch()
                    ).count()
                );
                return true;
            }
        }
        return false;
    }

    ExecutionReport record_fill(uint64_t order_id, double fill_price, uint32_t fill_qty) {
        ActiveOrder* ord = nullptr;
        for (size_t i = 0; i < active_order_count_; ++i) {
            auto* o = active_orders_[i];
            if (o && o->request.order_id == order_id) {
                ord = o;
                break;
            }
        }
        if (!ord || execution_history_count_ >= MAX_EXECUTION_REPORTS) return {};

        uint32_t price_paise = static_cast<uint32_t>(fill_price * 100.0 + 0.5);

        ord->filled_quantity += fill_qty;
        if (ord->filled_quantity >= ord->request.quantity) {
            ord->status = OrderStatus::FILLED;
        } else {
            ord->status = OrderStatus::PARTIALLY_FILLED;
        }

        ExecutionReport* rpt = execution_report_pool_.allocate();
        *rpt = ExecutionReport{};
        rpt->order_id = order_id;
        rpt->exec_id = next_exec_id_++;
        rpt->instrument_token = ord->request.instrument_token;
        safe_copy_str(rpt->symbol, ord->request.symbol, sizeof(rpt->symbol));
        rpt->side = ord->request.side;
        rpt->status = ord->status;
        rpt->fill_price_paise = price_paise;
        rpt->fill_quantity = fill_qty;
        rpt->cum_quantity = ord->filled_quantity;
        rpt->leaves_quantity = ord->request.quantity - ord->filled_quantity;
        rpt->timestamp_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now().time_since_epoch()
            ).count()
        );

        execution_history_[execution_history_count_++] = rpt;
        return *rpt;
    }

    /**
     * @brief Formats order for Zerodha Kite Connect REST API specification.
     * Zerodha POST /orders/regular requests use application/x-www-form-urlencoded payload,
     * while responses are returned in JSON format.
     */
    static std::string serialize_kite_rest_payload(const OrderRequest& req) {
        std::ostringstream ss;
        ss << "tradingsymbol=" << req.symbol
           << "&exchange=NFO"
           << "&transaction_type=" << (req.side == OrderSide::BUY ? "BUY" : "SELL")
           << "&order_type=" << (req.type == OrderType::MARKET ? "MARKET" : "LIMIT")
           << "&quantity=" << req.quantity
           << "&product=MIS"
           << "&price=" << std::fixed << std::setprecision(2) << (static_cast<double>(req.price_paise) / 100.0)
           << "&validity=DAY";
        return ss.str();
    }

    [[nodiscard]] std::span<ActiveOrder* const> get_orders() const noexcept {
        return {active_orders_.data(), active_order_count_};
    }

    [[nodiscard]] std::span<ExecutionReport* const> get_executions() const noexcept {
        return {execution_history_.data(), execution_history_count_};
    }

private:
    static constexpr size_t MAX_ACTIVE_ORDERS = 4096;
    static constexpr size_t MAX_EXECUTION_REPORTS = 4096;

    uint64_t next_order_id_;
    uint64_t next_exec_id_;

    ObjectPool<ActiveOrder, MAX_ACTIVE_ORDERS> order_pool_;
    ObjectPool<ExecutionReport, MAX_EXECUTION_REPORTS> execution_report_pool_;
    std::array<ActiveOrder*, MAX_ACTIVE_ORDERS> active_orders_{};
    std::array<ExecutionReport*, MAX_EXECUTION_REPORTS> execution_history_{};
    size_t active_order_count_{0};
    size_t execution_history_count_{0};
};

} // namespace hft

#endif // ORDER_MANAGER_HPP
