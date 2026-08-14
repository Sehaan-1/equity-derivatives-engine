#ifndef LATENCY_TRACKER_HPP
#define LATENCY_TRACKER_HPP

#include <chrono>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cstdint>
#include <string>

namespace hft {

/**
 * @brief High-precision Nanosecond Latency Tracker & Histogram
 */
class LatencyTracker {
public:
    explicit LatencyTracker(size_t max_samples = 1000000) {
        samples_.reserve(max_samples);
    }

    inline void record_nanos(uint64_t nanos) noexcept {
        if (samples_.size() < samples_.capacity()) {
            samples_.push_back(nanos);
        }
    }

    struct LatencyStats {
        uint64_t count{0};
        double min_ns{0.0};
        double max_ns{0.0};
        double mean_ns{0.0};
        double p50_ns{0.0};
        double p90_ns{0.0};
        double p99_ns{0.0};
        double p99_9_ns{0.0};
    };

    LatencyStats calculate_stats() const {
        if (samples_.empty()) return {};

        std::vector<uint64_t> sorted = samples_;
        std::sort(sorted.begin(), sorted.end());

        LatencyStats s{};
        s.count = sorted.size();
        s.min_ns = static_cast<double>(sorted.front());
        s.max_ns = static_cast<double>(sorted.back());

        double sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
        s.mean_ns = sum / s.count;

        s.p50_ns = static_cast<double>(sorted[static_cast<size_t>(s.count * 0.50)]);
        s.p90_ns = static_cast<double>(sorted[static_cast<size_t>(s.count * 0.90)]);
        s.p99_ns = static_cast<double>(sorted[static_cast<size_t>(s.count * 0.99)]);
        s.p99_9_ns = static_cast<double>(sorted[static_cast<size_t>(s.count * 0.999)]);

        return s;
    }

    void clear() noexcept {
        samples_.clear();
    }

private:
    std::vector<uint64_t> samples_;
};

/**
 * @brief RAII High-Resolution Timer for measuring microsecond/nanosecond scope executions
 */
class ScopedTimer {
public:
    explicit ScopedTimer(LatencyTracker& tracker)
        : tracker_(tracker), start_(std::chrono::high_resolution_clock::now()) {}

    ~ScopedTimer() {
        auto end = std::chrono::high_resolution_clock::now();
        uint64_t elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start_).count();
        tracker_.record_nanos(elapsed);
    }

private:
    LatencyTracker& tracker_;
    std::chrono::high_resolution_clock::time_point start_;
};

} // namespace hft

#endif // LATENCY_TRACKER_HPP
