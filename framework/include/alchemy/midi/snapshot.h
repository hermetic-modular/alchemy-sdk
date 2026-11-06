#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace alchemy::midi {

// One publisher and one reader. A lease keeps its slot immutable until released.
template <typename T, size_t Slots = 3>
class SnapshotExchange {
    static_assert(Slots >= 3 && Slots < 0xffffffffu, "Snapshot storage needs at least three slots");
    static_assert(std::is_trivially_copyable<T>::value, "Snapshots must be value objects");
    static_assert(std::atomic<uint32_t>::is_always_lock_free, "Publication needs lock-free 32-bit atomics");
    enum : uint32_t { Free, Writing, Ready, Reading };
    struct Slot {
        std::atomic<uint32_t> state{Free};
        T value{};
        uint32_t revision = 0;
    };

  public:
    class Lease {
      public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        ~Lease() { Release(); }
        const T* Get() const { return slot_ ? &slot_->value : nullptr; }
        const T& operator*() const { return slot_->value; }
        const T* operator->() const { return Get(); }
        explicit operator bool() const { return slot_ != nullptr; }
        uint32_t Revision() const { return slot_ ? slot_->revision : 0; }
        void Release() {
            if (slot_) slot_->state.store(Free, std::memory_order_release);
            slot_ = nullptr;
        }
      private:
        friend class SnapshotExchange;
        Slot* slot_ = nullptr;
    };

    template <typename Validate>
    bool Publish(const T& value, Validate&& validate) {
        if (!validate(value)) return false;
        for (size_t i = 0; i < Slots; ++i) {
            uint32_t expected = Free;
            if (!slots_[i].state.compare_exchange_strong(expected, Writing,
                    std::memory_order_acquire, std::memory_order_relaxed)) continue;
            auto& slot = slots_[i];
            slot.value = value;
            if (++revision_ == 0) ++revision_;
            slot.revision = revision_;
            slot.state.store(Ready, std::memory_order_release);
            const uint32_t previous = latest_.exchange(static_cast<uint32_t>(i + 1), std::memory_order_acq_rel);
            if (previous) {
                expected = Ready;
                slots_[previous - 1].state.compare_exchange_strong(expected, Free,
                    std::memory_order_release, std::memory_order_relaxed);
            }
            return true;
        }
        return false;
    }

    bool Acquire(Lease& lease) {
        if (lease) return false;
        const uint32_t index = latest_.exchange(0, std::memory_order_acq_rel);
        if (!index) return false;
        auto& slot = slots_[index - 1];
        uint32_t expected = Ready;
        if (!slot.state.compare_exchange_strong(expected, Reading,
                std::memory_order_acquire, std::memory_order_relaxed)) return false;
        lease.slot_ = &slot;
        return true;
    }

  private:
    std::array<Slot, Slots> slots_{};
    std::atomic<uint32_t> latest_{0};
    uint32_t revision_ = 0;
};

} // namespace alchemy::midi
