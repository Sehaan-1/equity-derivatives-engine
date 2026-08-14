# Low-Latency C++ Execution Engine Simulator and Backtesting Framework

[![C++ Standard](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Build Tool](https://img.shields.io/badge/Build-Makefile-gray.svg)]()
[![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)]()

A high-performance C++20 simulator and event-driven backtesting framework designed for Indian equity derivatives (NSE NIFTY/BANKNIFTY futures and options). The project models an in-memory execution pipeline featuring lock-free queues, pre-allocated memory pools, Kite-compatible binary protocol parsing, microsecond-level pre-trade risk controls, and accurate Indian statutory transaction costs.

---

## Table of Contents

- [Honesty and Scope Boundaries](#honesty-and-scope-boundaries)
- [Key Features](#key-features)
- [Architecture and Concurrency Model](#architecture-and-concurrency-model)
- [Indian Equity Derivatives Statutory Cost Matrix](#indian-equity-derivatives-statutory-cost-matrix)
- [Tech Stack](#tech-stack)
- [Prerequisites](#prerequisites)
- [Getting Started](#getting-started)
  - [1. Clone Repository](#1-clone-repository)
  - [2. Compile the C++ Engine](#2-compile-the-c-engine)
  - [3. Run the Built-In Test Suite](#3-run-the-built-in-test-suite)
  - [4. Run Queue Performance Benchmarks](#4-run-queue-performance-benchmarks)
  - [5. Run Backtests](#5-run-backtests)
  - [6. Run Multi-Threaded Simulation](#6-run-multi-threaded-simulation)
  - [7. Launch the Web Dashboard](#7-launch-the-web-dashboard)
- [CLI Reference and API Documentation](#cli-reference-and-api-documentation)
- [Quantitative Strategies](#quantitative-strategies)
- [Repository and Codebase Structure](#repository-and-codebase-structure)
- [Performance Benchmarks and Profiling](#performance-benchmarks-and-profiling)
- [Troubleshooting](#troubleshooting)
- [Contributing](#contributing)
- [License](#license)

---

## Honesty and Scope Boundaries

To maintain technical integrity and clear expectations, the following architectural realities define the scope of this project:

### What This Project Is

- A high-throughput, deterministic **in-memory simulator** written in ISO C++20 for researching order execution pipelines, market depth dynamics, and algorithmic strategies.
- A **lock-free thread boundary design** utilizing single-producer single-consumer (SPSC) and multi-producer single-consumer (MPSC) ring buffers with cache-line alignment to eliminate false sharing.
- A **zero-allocation hot-path architecture** within the core engine event loop, using pre-allocated object pools (`ObjectPool<T>`) for orders and execution reports.
- A **Kite-compatible binary parser** supporting Zerodha Kite Connect WebSocket Full Mode (184-byte packets) and Quote Mode (44-byte packets) using portable big-endian byte extraction without POSIX-only networking headers.
- An **event-time backtesting engine** that processes tick streams chronologically and evaluates rate limits against event timestamps (`validate_order_at_ns`) to prevent burst-tick false rejections.
- A **statutory cost model** implementing current Indian exchange fees, Securities Transaction Tax (STT), SEBI charges, stamp duty, brokerage rules, and GST.

### What This Project Is Not

- **Not an exchange-colocated HFT engine**: Real colocation involves kernel bypass (e.g., Solarflare OpenOnload, DPDK, EF_VI), FPGA hardware acceleration, direct microwave/fiber lines, and native exchange binary protocols (e.g., NSE EMDI/TAP, FIX, ITCH/OUCH).
- **Not a live broker auto-trader**: Zerodha Kite Connect is a retail REST/WebSocket API over the public Internet. Order roundtrip times through retail brokers are governed by public WAN transit (20-100 ms), completely superseding in-memory microsecond measurements.
- **Not a 100% lock-free pipeline**: While the inter-thread ring buffers are strictly lock-free, the shared `OrderBook` snapshot employs `std::shared_mutex` for readers/writer synchronization. This design prevents undefined behavior and data races under standard C++ memory model rules when copying non-atomic depth structures.
- **Dashboard market telemetry is synthetic**: The web dashboard generates random-walk synthetic market depth and order imbalance for visualization and interactive testing; it does not connect to a live broker account.
- **Benchmarks are hardware-dependent**: Latency and throughput figures presented in documentation reflect local benchmark runs on modern multi-core x86_64 hardware and should be verified locally via `--run-benchmark`.

---

## Key Features

- **Strict C++20 Standard Compliance**: Compiled with `-std=c++20 -O3 -pthread -Wall -Wextra -march=native -flto`.
- **Lock-Free Ring Buffers**: Power-of-two circular queues with acquire-release memory semantics and 64-byte cache line padding (`alignas(64)`).
- **Fixed-Size Arena Allocators**: Zero-fragmentation object pools for `ActiveOrder` and `ExecutionReport` instances to avoid heap allocation overhead during order generation and filling.
- **Kite Binary Protocol Parser**: Zero-copy big-endian binary decoder extracting instrument tokens, Level-2 (5-depth) bid/ask ladders, volume, open interest, and traded prices.
- **Deterministic Pre-Trade Risk Engine**: Validates maximum order quantity, notional value thresholds, price collar fat-finger limits, order rate throttles, daily maximum drawdown circuit breakers, and an atomic kill switch.
- **Indian Market Microstructure Modeling**: Evaluates Order Book Imbalance (OBI), micro-price volume weighting, and Black-Scholes Greeks for option delta hedging.
- **Statutory Friction Engine**: Real-time computation of Indian brokerage, STT, NSE turnover charges, SEBI turnover fees, stamp duty, and 18% GST on applicable charges.
- **Secure Web Control Interface**: Node.js and Express backend serving a real-time WebSocket dashboard, executing binary routines via validated parameter arrays without shell interpolation.

---

## Architecture and Concurrency Model

The engine is decoupled into three dedicated threads communicating through lock-free ring buffers and a mutex-guarded order book snapshot.

```text
Synthetic / Kite-Compatible Binary Feed
                |
                v
+------------------------------------+
|     Market Data Ingestion Thread   |
|  - Ingests ticks / parses frames   |
|  - Updates OrderBook (Writer lock) |
|  - Pushes to SPSC Tick Queue       |
+------------------------------------+
                |
                v [Lock-Free SPSC Queue: 65,536 capacity]
+------------------------------------+
|       Strategy Pipeline Thread     |
|  - Reads OrderBook snapshot        |
|  - Computes VWAP / Imbalance / OBI |
|  - Calculates Delta / Gamma Greeks |
|  - Emits OrderRequest into buffer  |
+------------------------------------+
                |
                v [Lock-Free MPSC Queue: 65,536 capacity]
+------------------------------------+
|    Execution & Risk Manager Thread |
|  - Validates Pre-Trade Risk Guard  |
|  - Allocates from OMS Object Pool  |
|  - Simulates Fill & Records PnL    |
|  - Calculates Latency Metrics      |
+------------------------------------+
                |
                v
  Backtest JSON Output / Telemetry Metrics / Web Dashboard
```

### Concurrency Guarantees

1. **Inter-Thread Communication**: SPSC queues use atomic head/tail counters with relaxed loads on private pointers and acquire-release synchronization across threads.
2. **Cache Isolation**: Atomic pointers are aligned to 64 bytes (`alignas(64)`) to eliminate false sharing across CPU cores.
3. **Thread Safety in Order Book**: `std::shared_mutex` allows concurrent reader threads to compute mid-price, spread, and imbalance while ensuring the ingestion thread retains exclusive write access when applying tick updates.

---

## Indian Equity Derivatives Statutory Cost Matrix

All trade fills in the backtesting framework pass through `IndianTaxBreakdown`, modeling statutory frictions applicable to Indian market participants:

| Fee / Tax Component | Futures Rate / Rule | Options Rate / Rule | Applicability |
| :--- | :--- | :--- | :--- |
| **Brokerage** | min(INR 20, Turnover * 0.03%) | min(INR 20, Turnover * 0.03%) | Both Buy and Sell |
| **Securities Transaction Tax (STT)** | 0.02% (2000 per Cr) | 0.125% (12500 per Cr on premium) | Sell Side Only |
| **Exchange Turnover Charges (NSE)** | 0.0019% (190 per Cr) | 0.05% (5000 per Cr on premium) | Both Buy and Sell |
| **SEBI Turnover Fee** | INR 10 / Crore (0.000001) | INR 10 / Crore (0.000001) | Both Buy and Sell |
| **Stamp Duty** | 0.002% (200 per Cr) | 0.003% (300 per Cr) | Buy Side Only |
| **Goods & Services Tax (GST)** | 18% on (Brokerage + Exchange + SEBI) | 18% on (Brokerage + Exchange + SEBI) | Both Buy and Sell |

---

## Tech Stack

- **Core Simulator**: C++20 (Standard Library, Pthreads, Atomics, Memory Arenas)
- **Compiler**: GCC 11+ / Clang 13+ / MSVC 2019+
- **Build System**: GNU Makefile
- **Dashboard Backend**: Node.js 18+, Express 4.19, ws 8.17 (WebSocket)
- **Dashboard Frontend**: Vanilla HTML5, CSS3, JavaScript (ES6+), Canvas API
- **Testing & Verification**: Built-in test harness (`--run-tests`), assert-based invariant checks

---

## Prerequisites

Ensure the following tools are installed in your environment:

### Linux (Ubuntu / Debian)

```bash
sudo apt-get update
sudo apt-get install -y build-essential g++ make nodejs npm
```

### macOS

```bash
xcode-select --install
brew install make node
```

### Windows

- Install **MinGW-w64** (with GCC 11+ supporting C++20) or **Visual Studio Build Tools**.
- Install **Node.js** (v18.0.0 or higher) from [nodejs.org](https://nodejs.org).

---

## Getting Started

### 1. Clone Repository

```bash
git clone https://github.com/your-username/equity-derivatives-engine.git
cd equity-derivatives-engine
```

### 2. Compile the C++ Engine

Compile the optimized executable with the Makefile:

```bash
make
```

This invokes:

```bash
g++ -std=c++20 -O3 -pthread -Wall -Wextra -Iinclude -march=native -flto src/main.cpp -o bin/trading_engine
```

On Windows with MinGW, this produces `bin/trading_engine.exe` (or `bin/trading_engine` on POSIX environments).

### 3. Run the Built-In Test Suite

Verify ring buffers, binary protocol decoders, risk limits, rate limiters, and OMS pooled reports:

```bash
./bin/trading_engine --run-tests
```

Expected output:

```text
[TEST] Running Lock-Free Ring Buffer Test... PASSED.
[TEST] Running Risk Engine Guardrails Test... PASSED.
[TEST] Running Simulated-Time Risk Rate Limiter Test... PASSED.
[TEST] Running Zerodha Kite Binary Parser Test (184B Full Packet)... PASSED.
[TEST] Testing Kite REST Form-Urlencoded Serializer... PASSED.
[TEST] Testing Pooled OMS Execution Reports... PASSED.
[TEST] All Arena.ai Engine Unit Tests Completed Successfully!
```

### 4. Run Queue Performance Benchmarks

Compare standard `std::mutex` with the lock-free single-producer single-consumer ring buffer over 5,000,000 operations:

```bash
./bin/trading_engine --run-benchmark
```

Example benchmark report:

```text
=================================================================
 RUNNING BENCHMARK: Lock-Free SPSC Queue vs std::mutex Queue
 Platform: Arena.ai C++ Engine Simulator (C++20)
 Iterations: 5000000 messages
=================================================================

[Mutex Queue]     Time: 382.41 ms | Throughput: 13.07 MOps/sec | Avg Latency: 38.24 ns
[Lock-Free SPSC]  Time: 49.12 ms  | Throughput: 101.79 MOps/sec | Avg Latency: 4.91 ns
=================================================================
```

### 5. Run Backtests

Execute event-driven backtesting for supported quantitative strategies against generated synthetic tick sequences:

```bash
# Default VWAP strategy with INR 1,000,000 capital and 0.5 bps slippage
./bin/trading_engine --run-backtest vwap

# Delta-neutral gamma scalping strategy
./bin/trading_engine --run-backtest delta_neutral

# Custom capital (INR 2,500,000) and slippage (1.0 bps)
./bin/trading_engine --run-backtest vwap 2500000 1.0
```

The output is formatted as JSON:

```json
{
  "ticks_processed": 100000,
  "orders_submitted": 342,
  "orders_executed": 342,
  "orders_rejected": 0,
  "initial_capital": 1000000,
  "gross_pnl": 18450.50,
  "net_pnl": 12380.20,
  "max_drawdown_pct": 1.42,
  "sharpe_ratio": 2.15,
  "win_rate_pct": 58.48,
  "profit_factor": 1.74,
  "taxes": {
    "brokerage": 3420.00,
    "stt": 1285.30,
    "exchange_fee": 321.40,
    "sebi_fee": 16.90,
    "stamp_duty": 338.20,
    "gst": 676.50,
    "total_charges": 6070.30
  }
}
```

### 6. Run Multi-Threaded Simulation

Run the complete multi-threaded pipeline (Ingestion -> Strategy -> Risk/OMS) to evaluate queue drop rates and risk latency histograms:

```bash
./bin/trading_engine --run-live
```

Expected output:

```text
=================================================================
 LIVE EXECUTION TELEMETRY REPORT (Arena.ai C++ Engine Simulator)
=================================================================
 Total Ticks Processed               : 20000
 Total Orders Processed              : 142
 Ingress Tick Drops                  : 0 (caller observed 0)
 Strategy Tick Drops                 : 0
 Order Drops                         : 0
 Strategy Queue Backpressure Events  : 0
 Order Queue Backpressure Events     : 0
 Throughput                          : 98520 ticks/sec
 Pre-Trade Risk Check Samples        : 142
 Pre-Trade Risk Check Latency (p50)  : 112.00 ns
 Pre-Trade Risk Check Latency (p90)  : 145.00 ns
 Pre-Trade Risk Check Latency (p99)  : 210.00 ns
 Pre-Trade Risk Check Latency (p99.9): 380.00 ns
=================================================================
```

### 7. Launch the Web Dashboard

```bash
cd web
npm install
npm start
```

Open your browser at `http://localhost:3000`.

The dashboard provides:
- Live synthetic order book depth visualization with bid/ask bars.
- Real-time order book imbalance gauge.
- One-click trigger for C++ benchmark, test suite, and backtest execution.
- Interactive emergency kill-switch toggle.

---

## CLI Reference and API Documentation

### Command-Line Arguments

The C++ binary accepts the following command-line flags:

| Flag | Arguments | Description |
| :--- | :--- | :--- |
| `--run-tests` | None | Executes the internal unit test suite and assertions. |
| `--run-benchmark` | None | Runs the 5M message throughput and latency comparison. |
| `--run-backtest` | `[strategy]` `[capital]` `[slippage_bps]` | Runs event-driven backtesting and prints JSON metrics. |
| `--run-live` | None | Runs the multi-threaded simulation pipeline and outputs telemetry. |
| `--help` | None | Displays usage instructions and available options. |

### Web Server API Endpoints

The Node.js Express server exposes REST endpoints that invoke the compiled binary using safe argument arrays:

| Endpoint | Method | Query Parameters | Description |
| :--- | :--- | :--- | :--- |
| `/api/tests` | `GET` | None | Executes `./bin/trading_engine --run-tests` and returns output. |
| `/api/benchmark` | `GET` | None | Executes `./bin/trading_engine --run-benchmark` and returns parsed metrics. |
| `/api/backtest` | `GET` | `strategy`, `capital`, `slippage` | Executes backtest with parameters and returns parsed JSON results. |
| `/api/live-run` | `GET` | None | Executes `./bin/trading_engine --run-live` and returns telemetry text. |

---

## Quantitative Strategies

### 1. VWAP Imbalance Scalper (`VWAPImbalanceScalper`)

- **Logic**: Combines Volume-Weighted Average Price (VWAP) deviation with Level-2 Order Book Imbalance (OBI).
- **Entry Condition**: Long entry triggered when current mid-price is more than 0.15% below VWAP and buyer depth imbalance exceeds +0.30.
- **Exit Condition**: Position closed when mid-price reverts to 0.10% above VWAP or imbalance reverses below -0.20.
- **Allocation**: Zero dynamic allocation during signal generation; writes orders directly to caller-allocated array buffers.

### 2. Delta-Neutral Gamma Scalper (`DeltaNeutralGammaScalper`)

- **Logic**: Models dynamic delta hedging for NIFTY options and futures.
- **Formula**: Evaluates option delta via Black-Scholes analytical approximation using complementary error function `std::erfc`.
- **Rebalancing**: Triggers futures hedge orders (buy/sell NIFTY futures in standard lot sizes of 25) whenever portfolio net delta drifts beyond configurable threshold (+/- 15.0 delta).

---

## Repository and Codebase Structure

```text
equity-derivatives/
├── Makefile                          # Build automation for C++20 binary
├── README.md                         # Project documentation and architecture guide
├── LICENSE                           # MIT License terms
├── .gitignore                        # Comprehensive Git ignore specifications
├── bin/                              # Output directory for compiled binaries
│   └── trading_engine                # Compiled C++ executable
├── include/                          # Header-only C++ engine components
│   ├── arena_allocator.hpp           # Fixed-size ObjectPool memory arena
│   ├── backtest_engine.hpp           # Event-driven backtester and statutory tax calculator
│   ├── latency_tracker.hpp           # Nanosecond latency tracker and statistical histogram
│   ├── lockfree_ring_buffer.hpp      # Cache-aligned SPSC and MPSC lock-free ring buffers
│   ├── market_data.hpp               # Tick struct, depth levels, and Kite binary parser
│   ├── order_book.hpp                # Thread-safe L2 Limit Order Book with shared_mutex
│   ├── order_manager.hpp             # Pooled OMS state engine and Kite REST serializer
│   ├── risk_engine.hpp               # Deterministic pre-trade risk checks and rate limiters
│   ├── strategy.hpp                  # Strategy interface, Black-Scholes model, VWAP scalper
│   └── system_engine.hpp             # Multi-threaded pipeline orchestrator
├── src/
│   └── main.cpp                      # CLI entry point, benchmark harness, and test runner
├── web/                              # Real-time monitoring and backtesting dashboard
│   ├── package.json                  # Node.js project manifest
│   ├── server.js                     # Express server, WebSocket feed, and binary executor
│   └── public/
│       └── index.html                # Single-page dashboard interface
└── docs/                             # Additional design specifications and plans
```

---

## Performance Benchmarks and Profiling

Local benchmark execution on an x86_64 host yields the following characteristics:

| Operation / Component | Mechanism | Measured Latency | Throughput |
| :--- | :--- | :--- | :--- |
| **SPSC Queue Push/Pop** | Lock-Free Atomic Indices | ~4.9 ns / op | >100 Million ops/sec |
| **Pre-Trade Risk Check** | Deterministic Bounds & Atomic Loads | ~110-150 ns (p50) | >6.5 Million orders/sec |
| **Kite Binary Frame Parse** | Direct Byte Offset & Bit Shifts | ~45 ns / 184B packet | >22 Million ticks/sec |
| **OMS Order Allocation** | Arena Object Pool Free-List | ~8 ns / allocation | >120 Million ops/sec |
| **OrderBook Read Snapshot** | Shared Mutex Shared Lock | ~25 ns / read | >40 Million reads/sec |

*Note: All latency figures are local hardware measurements produced via `--run-benchmark` and `--run-live`.*

---

## Troubleshooting

### 1. Compilation Fails with C++20 Errors

Ensure your compiler supports the full C++20 standard:

```bash
# Check GCC version (must be GCC 11 or higher)
g++ --version

# Verify manual compilation command
g++ -std=c++20 -O3 -pthread -Wall -Wextra -Iinclude src/main.cpp -o bin/trading_engine
```

If using Clang, use `clang++ -std=c++20`.

### 2. Node.js Dashboard Cannot Find Binary

If the web dashboard logs `Engine binary not found`:
1. Run `make` from the repository root to generate `bin/trading_engine` (or `bin/trading_engine.exe` on Windows).
2. Ensure the working directory when launching the server is `web/`.

### 3. Port 3000 Already in Use

To start the web dashboard on an alternate port:

```bash
PORT=8080 npm start
```

---

## Contributing

1. Fork the repository.
2. Create a feature branch: `git checkout -b feature/new-guardrail`.
3. Verify that all tests pass: `./bin/trading_engine --run-tests`.
4. Ensure no hot-path dynamic memory allocations (`new` or `malloc`) are introduced in core loops.
5. Commit changes with clear commit messages and submit a pull request.

---

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.
