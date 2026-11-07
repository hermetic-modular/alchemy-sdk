#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "alchemy/midi/queue.h"
#include "alchemy/control/tick_timebase.h"

namespace alchemy::midi {
struct StampedByte { uint32_t tick = 0; uint8_t byte = 0; };

template<size_t Capacity> class RawInput {
  public:
    bool Receive(const uint8_t* bytes, size_t size, uint32_t raw_tick) {
        if (lost_.load(std::memory_order_acquire)) return false;
        if (!bytes && size) { lost_.store(true, std::memory_order_release); return false; }
        for (size_t i = 0; i < size; ++i) {
            if (!bytes_.Push(StampedByte{raw_tick, bytes[i]})) {
                lost_.store(true, std::memory_order_release);
                return false;
            }
        }
        return true;
    }
    void MarkLost() { lost_.store(true, std::memory_order_release); }
    bool DiscardLost() {
        if (!lost_.load(std::memory_order_acquire)) return false;
        bytes_.Clear();
        ++recoveries_;
        return true;
    }
    // Foreground only, with the producer quiesced.
    void Restart() { bytes_.Clear(); lost_.store(false, std::memory_order_release); }
    bool Pop(StampedByte& byte) { return bytes_.Pop(byte); }
    bool Peek(StampedByte& byte) const { return bytes_.Peek(byte); }
    size_t Size() const { return bytes_.Size(); }
    uint32_t Recoveries() const { return recoveries_; }
    bool Lost() const { return lost_.load(std::memory_order_acquire); }
  private:
    Queue<StampedByte, Capacity> bytes_;
    std::atomic<bool> lost_{false};
    uint32_t recoveries_ = 0;
};

inline bool ArrivalTime(uint32_t observed_tick, TickTimebase::Sample now,
                        uint32_t& result) {
    return now.TimeOf(observed_tick, result);
}
}
