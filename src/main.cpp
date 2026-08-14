#include "lockfree_ring_buffer.hpp"
#include "arena_allocator.hpp"
#include "market_data.hpp"
#include "order_book.hpp"
#include "risk_engine.hpp"
#include "order_manager.hpp"
#include "strategy.hpp"
#include "backtest_engine.hpp"
#include "latency_tracker.hpp"
#include "system_engine.hpp"

#include <iostream>
#include <vector>
#include <string>
#include <random>
#include <cmath>
#include <chrono>
#include <queue>
#include <mutex>
#include <cassert>

using namespace hft;

// Helper to generate realistic NIFTY Derivatives Ticks
std::vector<Tick> generate_synthetic_nifty_ticks(size_t count = 50000, double base_price = 24500.0) {
    std::vector<Tick> ticks;
    ticks.reserve(count);

    std::mt19937 rng(42);
    std::normal_distribution<double> dist(0.0, 1.2);
    std::uniform_int_distribution<uint32_t> qty_dist(25, 250);
    std::uniform_real_distribution<double> noise_dist(-0.35, 0.35);

    double current_price = base_price;
    uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()
        ).count()
    );

    double rolling_imbalance = 0.0;

    for (size_t i = 0; i < count; ++i) {
        double price_delta = dist(rng);
        current_price += price_delta;
        if (current_price < 1000.0) current_price = 1000.0;

        // Auto-regressive micro-structure imbalance with noise and order flow momentum
        rolling_imbalance = 0.7 * rolling_imbalance - 0.25 * price_delta + noise_dist(rng);
        rolling_imbalance = std::clamp(rolling_imbalance, -0.75, 0.75);

        double bid_weight = 1.0 + rolling_imbalance;
        double ask_weight = 1.0 - rolling_imbalance;

        Tick tick{};
        tick.instrument_token = 123456;
        tick.timestamp_ns = now_ns + i * 1000000;
        tick.ltp = static_cast<uint32_t>(current_price * 100.0);
        tick.last_quantity = qty_dist(rng);
        tick.volume = static_cast<uint32_t>(10000 + i * 5);
        safe_copy_str(tick.symbol, "NIFTY26AUGFUT", sizeof(tick.symbol));

        double spread = 0.50;
        double bid0 = current_price - spread * 0.5;
        double ask0 = current_price + spread * 0.5;

        for (int d = 0; d < 5; ++d) {
            tick.depth.buy[d].price = static_cast<uint32_t>((bid0 - d * 0.25) * 100.0);
            uint32_t b_qty = static_cast<uint32_t>(qty_dist(rng) * (5 - d) * bid_weight);
            tick.depth.buy[d].quantity = (b_qty > 0) ? b_qty : 25;
            tick.depth.buy[d].orders = static_cast<uint16_t>(2 + d);

            tick.depth.sell[d].price = static_cast<uint32_t>((ask0 + d * 0.25) * 100.0);
            uint32_t a_qty = static_cast<uint32_t>(qty_dist(rng) * (5 - d) * ask_weight);
            tick.depth.sell[d].quantity = (a_qty > 0) ? a_qty : 25;
            tick.depth.sell[d].orders = static_cast<uint16_t>(2 + d);
        }

        ticks.push_back(tick);
    }

    return ticks;
}

// Benchmark Standard Mutex Queue vs Lock-Free SPSC Queue
void run_queue_benchmark() {
    constexpr size_t NUM_ITEMS = 5000000; // 5 Million messages
    std::cout << "\n=================================================================\n";
    std::cout << " RUNNING BENCHMARK: Lock-Free SPSC Queue vs std::mutex Queue\n";
    std::cout << " Platform: Arena.ai C++ Engine Simulator (C++20)\n";
    std::cout << " Iterations: " << NUM_ITEMS << " messages\n";
    std::cout << "=================================================================\n\n";

    // 1. Standard Mutex Queue
    {
        std::queue<uint64_t> m_queue;
        std::mutex m_mutex;

        auto start = std::chrono::high_resolution_clock::now();

        std::thread producer([&]() {
            for (uint64_t i = 0; i < NUM_ITEMS; ++i) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_queue.push(i);
            }
        });

        std::thread consumer([&]() {
            uint64_t popped = 0;
            while (popped < NUM_ITEMS) {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_queue.empty()) {
                    m_queue.pop();
                    popped++;
                }
            }
        });

        producer.join();
        consumer.join();

        auto end = std::chrono::high_resolution_clock::now();
        double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
        double mops = (NUM_ITEMS / (elapsed_ms / 1000.0)) / 1000000.0;
        double avg_lat_ns = (elapsed_ms * 1000000.0) / (NUM_ITEMS * 2);

        std::cout << "[Mutex Queue] Time: " << elapsed_ms << " ms | Throughput: " << mops
                  << " MOps/sec | Avg Latency: " << avg_lat_ns << " ns\n";
    }

    // 2. Lock-Free SPSC Queue
    {
        LockFreeSPSCQueue<uint64_t, 65536> lf_queue;

        auto start = std::chrono::high_resolution_clock::now();

        std::thread producer([&]() {
            for (uint64_t i = 0; i < NUM_ITEMS; ++i) {
                while (!lf_queue.push(i)) {
                    std::this_thread::yield();
                }
            }
        });

        std::thread consumer([&]() {
            uint64_t val = 0;
            uint64_t popped = 0;
            while (popped < NUM_ITEMS) {
                if (lf_queue.pop(val)) {
                    popped++;
                } else {
                    std::this_thread::yield();
                }
            }
        });

        producer.join();
        consumer.join();

        auto end = std::chrono::high_resolution_clock::now();
        double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
        double mops = (NUM_ITEMS / (elapsed_ms / 1000.0)) / 1000000.0;
        double avg_lat_ns = (elapsed_ms * 1000000.0) / (NUM_ITEMS * 2);

        std::cout << "[Lock-Free SPSC] Time: " << elapsed_ms << " ms | Throughput: " << mops
                  << " MOps/sec | Avg Latency: " << avg_lat_ns << " ns\n";
    }

    std::cout << "=================================================================\n\n";
}

// Internal Unit Tests Function
void run_unit_tests() {
    std::cout << "[TEST] Running Lock-Free Ring Buffer Test...";
    LockFreeSPSCQueue<int, 16> q;
    for (int i = 0; i < 15; ++i) {
        assert(q.push(i));
    }
    int val = 0;
    for (int i = 0; i < 15; ++i) {
        assert(q.pop(val));
        assert(val == i);
    }
    std::cout << " PASSED.\n";

    std::cout << "[TEST] Running Risk Engine Guardrails Test...";
    RiskEngine risk;
    OrderRequest req{};
    req.quantity = 100;
    req.price_paise = 2450000;
    req.type = OrderType::LIMIT;

    assert(risk.validate_order(req, 2450000) == RiskCheckResult::PASSED);

    req.quantity = 5000; // Exceeds max 1800 limit
    assert(risk.validate_order(req, 2450000) == RiskCheckResult::REJECTED_MAX_QUANTITY);
    std::cout << " PASSED.\n";

    std::cout << "[TEST] Running Simulated-Time Risk Rate Limiter Test...";
    RiskEngine timed_risk(RiskEngine::RiskLimits{1800, 2500000.0, 3.0, 2, -50000.0});
    OrderRequest timed_req{};
    timed_req.quantity = 1;
    timed_req.price_paise = 2450000;
    timed_req.type = OrderType::LIMIT;
    assert(timed_risk.validate_order_at_ns(timed_req, 2450000, 1000000000ULL) == RiskCheckResult::PASSED);
    assert(timed_risk.validate_order_at_ns(timed_req, 2450000, 1000000001ULL) == RiskCheckResult::PASSED);
    assert(timed_risk.validate_order_at_ns(timed_req, 2450000, 1000000002ULL) == RiskCheckResult::REJECTED_RATE_LIMIT);
    assert(timed_risk.validate_order_at_ns(timed_req, 2450000, 2000000000ULL) == RiskCheckResult::PASSED);
    std::cout << " PASSED.\n";

    std::cout << "[TEST] Running Zerodha Kite Binary Parser Test (184B Full Packet)...";
    uint8_t dummy_pkt[188] = {0};
    dummy_pkt[0] = 0; dummy_pkt[1] = 1; // 1 packet
    dummy_pkt[2] = 0; dummy_pkt[3] = 184; // length 184

    // Instrument token = 123456
    uint32_t token = 123456;
    dummy_pkt[4] = (token >> 24) & 0xFF;
    dummy_pkt[5] = (token >> 16) & 0xFF;
    dummy_pkt[6] = (token >> 8) & 0xFF;
    dummy_pkt[7] = token & 0xFF;

    // LTP = 2450000 paise (24500.00)
    uint32_t ltp = 2450000;
    dummy_pkt[8] = (ltp >> 24) & 0xFF;
    dummy_pkt[9] = (ltp >> 16) & 0xFF;
    dummy_pkt[10] = (ltp >> 8) & 0xFF;
    dummy_pkt[11] = ltp & 0xFF;

    // Volume = 50000
    uint32_t vol = 50000;
    dummy_pkt[20] = (vol >> 24) & 0xFF;
    dummy_pkt[21] = (vol >> 16) & 0xFF;
    dummy_pkt[22] = (vol >> 8) & 0xFF;
    dummy_pkt[23] = vol & 0xFF;

    Tick tick_out[1];
    size_t count = KiteBinaryParser::parse_binary_frame(dummy_pkt, 188, tick_out, 1);
    assert(count == 1);
    assert(tick_out[0].instrument_token == 123456);
    assert(tick_out[0].ltp == 2450000);
    assert(tick_out[0].volume == 50000);
    std::cout << " PASSED.\n";

    std::cout << "[TEST] Testing Kite REST Form-Urlencoded Serializer...";
    OrderRequest rest_req{};
    safe_copy_str(rest_req.symbol, "NIFTY26AUGFUT", sizeof(rest_req.symbol));
    rest_req.side = OrderSide::BUY;
    rest_req.type = OrderType::LIMIT;
    rest_req.quantity = 25;
    rest_req.price_paise = 2450000;
    std::string payload = OrderManager::serialize_kite_rest_payload(rest_req);
    assert(payload.find("tradingsymbol=NIFTY26AUGFUT") != std::string::npos);
    assert(payload.find("transaction_type=BUY") != std::string::npos);
    assert(payload.find("price=24500.00") != std::string::npos);
    std::cout << " PASSED.\n";

    std::cout << "[TEST] Testing Pooled OMS Execution Reports...";
    OrderManager oms;
    uint64_t order_id = oms.create_order("NIFTY26AUGFUT", 123456, OrderSide::BUY, OrderType::LIMIT, 24500.0, 25);
    assert(order_id != 0);
    ExecutionReport report = oms.record_fill(order_id, 24500.0, 25);
    assert(report.order_id == order_id);
    assert(oms.get_orders().size() == 1);
    assert(oms.get_executions().size() == 1);
    assert(oms.get_executions()[0]->order_id == order_id);
    std::cout << " PASSED.\n";

    std::cout << "[TEST] All Arena.ai Engine Unit Tests Completed Successfully!\n\n";
}

// Backtest Runner Function
void run_backtest_cli(const std::string& strat_type = "vwap", double initial_capital = 1000000.0,
                      double slippage_bps = 0.5) {
    auto ticks = generate_synthetic_nifty_ticks(100000, 24500.0);
    BacktestEngine engine(initial_capital);

    BacktestResult res;
    if (strat_type == "delta_neutral") {
        DeltaNeutralGammaScalper strat;
        res = engine.run(strat, ticks, slippage_bps);
    } else {
        VWAPImbalanceScalper strat;
        res = engine.run(strat, ticks, slippage_bps);
    }

    std::cout << "{\n"
              << "  \"ticks_processed\": " << res.total_ticks_processed << ",\n"
              << "  \"orders_submitted\": " << res.total_orders_submitted << ",\n"
              << "  \"orders_executed\": " << res.total_orders_executed << ",\n"
              << "  \"orders_rejected\": " << res.total_orders_rejected << ",\n"
              << "  \"initial_capital\": " << res.initial_capital << ",\n"
              << "  \"gross_pnl\": " << res.gross_pnl << ",\n"
              << "  \"net_pnl\": " << res.net_pnl << ",\n"
              << "  \"max_drawdown_pct\": " << res.max_drawdown_pct << ",\n"
              << "  \"sharpe_ratio\": " << res.sharpe_ratio << ",\n"
              << "  \"win_rate_pct\": " << res.win_rate_pct << ",\n"
              << "  \"profit_factor\": " << res.profit_factor << ",\n"
              << "  \"taxes\": {\n"
              << "    \"brokerage\": " << res.tax_breakdown.brokerage << ",\n"
              << "    \"stt\": " << res.tax_breakdown.stt << ",\n"
              << "    \"exchange_fee\": " << res.tax_breakdown.exchange_fee << ",\n"
              << "    \"sebi_fee\": " << res.tax_breakdown.sebi_fee << ",\n"
              << "    \"stamp_duty\": " << res.tax_breakdown.stamp_duty << ",\n"
              << "    \"gst\": " << res.tax_breakdown.gst << ",\n"
              << "    \"total_charges\": " << res.tax_breakdown.total_charges << "\n"
              << "  }\n"
              << "}\n";
}

int main(int argc, char* argv[]) {
    std::string mode = "--help";
    if (argc > 1) mode = argv[1];

    if (mode == "--run-tests") {
        run_unit_tests();
    } else if (mode == "--run-benchmark") {
        run_queue_benchmark();
    } else if (mode == "--run-backtest") {
        std::string strat = (argc > 2) ? argv[2] : "vwap";
        double initial_capital = (argc > 3) ? std::stod(argv[3]) : 1000000.0;
        double slippage_bps = (argc > 4) ? std::stod(argv[4]) : 0.5;
        run_backtest_cli(strat, initial_capital, slippage_bps);
    } else if (mode == "--run-live") {
        std::cout << "[SYSTEM] Initializing Arena.ai Lock-Free Multi-Threaded Engine Simulator...\n";
        ExecutionEngine engine;
        engine.start();

        std::cout << "[SYSTEM] Injecting synthetic high-frequency NIFTY tick stream...\n";
        auto ticks = generate_synthetic_nifty_ticks(20000, 24500.0);

        auto start_time = std::chrono::high_resolution_clock::now();
        uint64_t failed_injections = 0;
        for (const auto& tick : ticks) {
            if (!engine.inject_tick(tick)) {
                failed_injections++;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        engine.stop();

        auto end_time = std::chrono::high_resolution_clock::now();
        double elapsed_sec = std::chrono::duration<double>(end_time - start_time).count();

        auto stats = engine.get_latency_tracker().calculate_stats();

        std::cout << "\n=================================================================\n";
        std::cout << " LIVE EXECUTION TELEMETRY REPORT (Arena.ai C++ Engine Simulator)\n";
        std::cout << "=================================================================\n";
        std::cout << " Total Ticks Processed    : " << engine.get_tick_count() << "\n";
        std::cout << " Total Orders Processed   : " << engine.get_orders_processed_count() << "\n";
        std::cout << " Ingress Tick Drops       : " << engine.get_ingress_tick_drop_count() << " (caller observed " << failed_injections << ")\n";
        std::cout << " Strategy Tick Drops      : " << engine.get_strategy_tick_drop_count() << "\n";
        std::cout << " Order Drops              : " << engine.get_order_drop_count() << "\n";
        std::cout << " Strategy Queue Backpressure Events : " << engine.get_strategy_queue_backpressure_count() << "\n";
        std::cout << " Order Queue Backpressure Events    : " << engine.get_order_queue_backpressure_count() << "\n";
        std::cout << " Throughput               : " << static_cast<uint64_t>(engine.get_tick_count() / elapsed_sec) << " ticks/sec\n";
        std::cout << " Pre-Trade Risk Check Samples       : " << stats.count << "\n";
        if (stats.count > 0) {
            std::cout << " Pre-Trade Risk Check Latency (p50)   : " << stats.p50_ns << " ns\n";
            std::cout << " Pre-Trade Risk Check Latency (p90)   : " << stats.p90_ns << " ns\n";
            std::cout << " Pre-Trade Risk Check Latency (p99)   : " << stats.p99_ns << " ns\n";
            std::cout << " Pre-Trade Risk Check Latency (p99.9) : " << stats.p99_9_ns << " ns\n";
        } else {
            std::cout << " Pre-Trade Risk Check Latency (p50)   : N/A (no orders triggered in run)\n";
        }
        std::cout << "=================================================================\n\n";
    } else {
        std::cout << "Lock-Free Multi-Threaded C++ Indian Equity Derivatives Engine Simulator (Arena.ai)\n";
        std::cout << "Usage:\n";
        std::cout << "  " << argv[0] << " --run-tests        : Run internal unit tests\n";
        std::cout << "  " << argv[0] << " --run-benchmark    : Compare Mutex vs Lock-Free Queue performance\n";
        std::cout << "  " << argv[0] << " --run-backtest [strategy] [capital] [slippage_bps] : Run event-driven backtest & output JSON report\n";
        std::cout << "  " << argv[0] << " --run-live         : Run multi-threaded live trading simulation\n";
    }

    return 0;
}
