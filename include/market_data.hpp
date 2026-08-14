#ifndef MARKET_DATA_HPP
#define MARKET_DATA_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <array>
#include <chrono>
#include <cstring>

namespace hft {

/**
 * @brief Zero-allocation, bounds-safe string copy helper for fixed-size char buffers.
 * Always guarantees null-termination and prevents -Wstringop-truncation warnings.
 */
inline void safe_copy_str(char* dest, std::string_view src, size_t dest_size) noexcept {
    if (!dest || dest_size == 0) return;
    size_t copy_len = (src.size() < dest_size - 1) ? src.size() : (dest_size - 1);
    for (size_t i = 0; i < copy_len; ++i) {
        dest[i] = src[i];
    }
    dest[copy_len] = '\0';
}

// Depth level structure (Bid / Ask) - 12 bytes
struct DepthLevel {
    uint32_t quantity{0};  // Bytes 0-3: Quantity
    uint32_t price{0};     // Bytes 4-7: Price in paise (e.g. 24500.50 -> 2450050)
    uint16_t orders{0};    // Bytes 8-9: Number of orders
    uint16_t reserved{0};  // Bytes 10-11: Reserved
};

// Top 5 Market Depth
struct MarketDepth {
    std::array<DepthLevel, 5> buy{};
    std::array<DepthLevel, 5> sell{};
};

// Instrument Type Enum
enum class InstrumentType : uint8_t {
    EQUITY = 0,
    FUTURES = 1,
    CALL_OPTION = 2,
    PUT_OPTION = 3
};

// Hot-path Market Tick Structure (Fixed-size)
struct alignas(64) Tick {
    uint32_t instrument_token{0};  // Zerodha Kite Instrument Token
    uint64_t timestamp_ns{0};     // Epoch Nanoseconds
    uint32_t ltp{0};               // Last Traded Price in paise (scaled by 100)
    uint32_t last_quantity{0};     // Last Traded Quantity
    uint32_t average_price{0};    // Average Traded Price
    uint32_t volume{0};            // Total Volume Traded
    uint32_t buy_quantity{0};      // Total Buy Quantity
    uint32_t sell_quantity{0};     // Total Sell Quantity
    uint32_t open{0};
    uint32_t high{0};
    uint32_t low{0};
    uint32_t close{0};
    uint32_t open_interest{0};     // Open Interest for Futures/Options
    InstrumentType type{InstrumentType::FUTURES};
    char symbol[24]{0};            // e.g., "NIFTY26AUGFUT", "NIFTY24500CE"
    MarketDepth depth{};           // L2 Depth

    [[nodiscard]] double get_ltp_double() const noexcept {
        return static_cast<double>(ltp) / 100.0;
    }
};

/**
 * @brief Zero-allocation parser for Zerodha Kite Connect WebSocket binary stream.
 * Pure C++ implementation without POSIX <arpa/inet.h> dependencies for cross-platform portability.
 */
class KiteBinaryParser {
public:
    /**
     * @brief Parses raw Kite binary websocket frame into Tick structs.
     * Offsets strictly conform to Zerodha Kite Connect WebSocket binary format (Full Mode 184 bytes).
     */
    static size_t parse_binary_frame(const uint8_t* buffer, size_t len, Tick* out_ticks, size_t max_ticks) noexcept {
        if (!buffer || len < 2 || max_ticks == 0) return 0;

        uint16_t num_packets = read_uint16_be(buffer);
        size_t offset = 2;
        size_t parsed_count = 0;

        uint64_t now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now().time_since_epoch()
            ).count()
        );

        for (uint16_t i = 0; i < num_packets && offset + 2 <= len && parsed_count < max_ticks; ++i) {
            uint16_t packet_len = read_uint16_be(buffer + offset);
            offset += 2;

            if (offset + packet_len > len) break; // Truncated buffer

            const uint8_t* pkt = buffer + offset;
            Tick& tick = out_ticks[parsed_count];
            tick = Tick{};

            tick.timestamp_ns = now_ns;
            tick.instrument_token = read_uint32_be(pkt + 0);

            if (packet_len == 8) { // Mode LTP (8 bytes)
                tick.ltp = read_uint32_be(pkt + 4);
            } else if (packet_len == 44 || packet_len == 184) { // Mode Quote (44B) or Full (184B)
                tick.ltp = read_uint32_be(pkt + 4);
                tick.last_quantity = read_uint32_be(pkt + 8);
                tick.average_price = read_uint32_be(pkt + 12);
                tick.volume = read_uint32_be(pkt + 16);
                tick.buy_quantity = read_uint32_be(pkt + 20);
                tick.sell_quantity = read_uint32_be(pkt + 24);
                tick.open = read_uint32_be(pkt + 28);
                tick.high = read_uint32_be(pkt + 32);
                tick.low = read_uint32_be(pkt + 36);
                tick.close = read_uint32_be(pkt + 40);

                if (packet_len == 184) { // Mode Full (184 bytes with L2 Depth)
                    tick.open_interest = read_uint32_be(pkt + 48);

                    // Market Depth starts at byte 64 (5 bids x 12B = 60B, then 5 asks x 12B = 60B)
                    const uint8_t* depth_ptr = pkt + 64;
                    for (int d = 0; d < 5; ++d) {
                        tick.depth.buy[d].quantity = read_uint32_be(depth_ptr + d * 12);
                        tick.depth.buy[d].price = read_uint32_be(depth_ptr + d * 12 + 4);
                        tick.depth.buy[d].orders = read_uint16_be(depth_ptr + d * 12 + 8);
                    }

                    depth_ptr += 60; // Offset to Asks
                    for (int d = 0; d < 5; ++d) {
                        tick.depth.sell[d].quantity = read_uint32_be(depth_ptr + d * 12);
                        tick.depth.sell[d].price = read_uint32_be(depth_ptr + d * 12 + 4);
                        tick.depth.sell[d].orders = read_uint16_be(depth_ptr + d * 12 + 8);
                    }
                }
            }

            offset += packet_len;
            parsed_count++;
        }

        return parsed_count;
    }

private:
    static inline uint32_t read_uint32_be(const uint8_t* ptr) noexcept {
        return (static_cast<uint32_t>(ptr[0]) << 24) |
               (static_cast<uint32_t>(ptr[1]) << 16) |
               (static_cast<uint32_t>(ptr[2]) << 8)  |
               (static_cast<uint32_t>(ptr[3]));
    }

    static inline uint16_t read_uint16_be(const uint8_t* ptr) noexcept {
        return (static_cast<uint16_t>(ptr[0]) << 8) | static_cast<uint16_t>(ptr[1]);
    }
};

} // namespace hft

#endif // MARKET_DATA_HPP
