#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace alchemy::midi {

// One producer and one consumer. Clear is a consumer operation.
template <typename T, size_t Capacity>
class Queue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Queue capacity must be a power of two");
    static_assert(Capacity <= 0x40000000u, "Queue capacity is too large");
    static_assert(std::is_trivially_copyable<T>::value, "Queue payload must be a value record");
    static_assert(std::atomic<uint32_t>::is_always_lock_free, "Queue requires lock-free indices");
  public:
    bool Push(const T& value) {
        const auto head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) == Capacity) return false;
        values_[head & (Capacity - 1)] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }
    bool Pop(T& value) {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        value = values_[tail & (Capacity - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }
    bool Peek(T& value) const {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        value = values_[tail & (Capacity - 1)];
        return true;
    }
    size_t Size() const {
        const auto tail = tail_.load(std::memory_order_acquire);
        const auto size = head_.load(std::memory_order_acquire) - tail;
        return size < Capacity ? size : Capacity;
    }
    void Clear() {
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
    }
    static constexpr size_t MaxSize() { return Capacity; }
  private:
    std::array<T, Capacity> values_{};
    alignas(32) std::atomic<uint32_t> head_{0};
    alignas(32) std::atomic<uint32_t> tail_{0};
};

} // namespace alchemy::midi
