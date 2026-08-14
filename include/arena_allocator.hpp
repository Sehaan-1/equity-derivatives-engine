#ifndef ARENA_ALLOCATOR_HPP
#define ARENA_ALLOCATOR_HPP

#include <cstddef>
#include <cstdint>
#include <vector>
#include <memory>
#include <type_traits>
#include <utility>
#include <cassert>

namespace hft {

/**
 * @brief Fixed-size Object Pool Allocator for ultra-low latency memory allocation.
 * Completely avoids system malloc/free and OS kernel calls on the trading hot path.
 */
template <typename T, size_t PoolSize = 4096>
class ObjectPool {
    union Node {
        Node* next;
        alignas(alignof(T)) char storage[sizeof(T)];
    };

public:
    ObjectPool() {
        pool_storage_ = std::make_unique<Node[]>(PoolSize);
        free_list_ = &pool_storage_[0];
        for (size_t i = 0; i < PoolSize - 1; ++i) {
            pool_storage_[i].next = &pool_storage_[i + 1];
        }
        pool_storage_[PoolSize - 1].next = nullptr;
    }

    ~ObjectPool() = default;

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    template <typename... Args>
    T* allocate(Args&&... args) {
        if (!free_list_) {
            // Hot path fallback or error handling
            throw std::bad_alloc();
        }
        Node* node = free_list_;
        free_list_ = node->next;
        T* obj = reinterpret_cast<T*>(node->storage);
        new (obj) T(std::forward<Args>(args)...);
        return obj;
    }

    void deallocate(T* ptr) noexcept {
        if (!ptr) return;
        ptr->~T();
        Node* node = reinterpret_cast<Node*>(ptr);
        node->next = free_list_;
        free_list_ = node;
    }

    [[nodiscard]] size_t capacity() const noexcept { return PoolSize; }

private:
    std::unique_ptr<Node[]> pool_storage_;
    Node* free_list_{nullptr};
};

} // namespace hft

#endif // ARENA_ALLOCATOR_HPP
