#ifndef LOCKFREE_RING_BUFFER_HPP
#define LOCKFREE_RING_BUFFER_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <new>

namespace hft {

constexpr size_t CACHE_LINE_SIZE = 64;

/**
 * @brief Lock-free Single-Producer Single-Consumer (SPSC) Ring Buffer.
 * Internal buffer is allocated on the heap to avoid thread stack overflow.
 */
template <typename T, size_t Capacity>
class LockFreeSPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");

public:
    LockFreeSPSCQueue() : head_(0), tail_(0) {
        // Allocate buffer on heap to prevent stack overflow
        buffer_ = std::make_unique<T[]>(Capacity);
    }

    ~LockFreeSPSCQueue() = default;

    LockFreeSPSCQueue(const LockFreeSPSCQueue&) = delete;
    LockFreeSPSCQueue& operator=(const LockFreeSPSCQueue&) = delete;

    template <typename... Args>
    bool emplace(Args&&... args) noexcept {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        const size_t current_head = head_.load(std::memory_order_acquire);

        if ((current_tail - current_head) >= Capacity) {
            return false; // Queue Full
        }

        buffer_[current_tail & MASK] = T(std::forward<Args>(args)...);
        tail_.store(current_tail + 1, std::memory_order_release);
        return true;
    }

    bool push(const T& item) noexcept {
        return emplace(item);
    }

    bool push(T&& item) noexcept {
        return emplace(std::move(item));
    }

    bool pop(T& item) noexcept {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        const size_t current_tail = tail_.load(std::memory_order_acquire);

        if (current_head == current_tail) {
            return false; // Queue Empty
        }

        item = std::move(buffer_[current_head & MASK]);
        head_.store(current_head + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] size_t size() const noexcept {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        return (current_tail >= current_head) ? (current_tail - current_head) : 0;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }

    [[nodiscard]] constexpr size_t capacity() const noexcept {
        return Capacity;
    }

private:
    static constexpr size_t MASK = Capacity - 1;

    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_{0};
    std::unique_ptr<T[]> buffer_;
};

/**
 * @brief Lock-Free Multi-Producer Single-Consumer (MPSC) Queue
 */
template <typename T, size_t Capacity>
class LockFreeMPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");

    struct Node {
        std::atomic<size_t> sequence;
        T data;
    };

public:
    LockFreeMPSCQueue() {
        buffer_ = std::make_unique<Node[]>(Capacity);
        for (size_t i = 0; i < Capacity; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
        enqueue_pos_.store(0, std::memory_order_relaxed);
        dequeue_pos_.store(0, std::memory_order_relaxed);
    }

    bool push(const T& data) noexcept {
        Node* node;
        size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            node = &buffer_[pos & MASK];
            size_t seq = node->sequence.load(std::memory_order_acquire);
            intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (dif == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (dif < 0) {
                return false; // Queue full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }

        node->data = data;
        node->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& data) noexcept {
        Node* node;
        size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            node = &buffer_[pos & MASK];
            size_t seq = node->sequence.load(std::memory_order_acquire);
            intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

            if (dif == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (dif < 0) {
                return false; // Queue empty
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }

        data = std::move(node->data);
        node->sequence.store(pos + MASK + 1, std::memory_order_release);
        return true;
    }

private:
    static constexpr size_t MASK = Capacity - 1;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> enqueue_pos_{0};
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> dequeue_pos_{0};
    std::unique_ptr<Node[]> buffer_;
};

} // namespace hft

#endif // LOCKFREE_RING_BUFFER_HPP
