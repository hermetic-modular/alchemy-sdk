#pragma once

#include "alchemy/midi/queue.h"
#include "alchemy/midi/types.h"

namespace alchemy::midi {

// One foreground producer, one audio consumer; port IDs are dense indices.
template <size_t Capacity, size_t Sources = 8>
class Ingress {
    static_assert(Sources > 0 && Sources <= 32, "Ingress supports 1..32 source ports");
  public:
    Ingress() {
        for (auto& session : failed_session_) session.store(0, std::memory_order_relaxed);
    }
    bool Push(Event event) {
        if (!event.Valid() || event.source.port >= Sources) return false;
        const auto port = event.source.port;
        const bool pending = pending_faults_.load(std::memory_order_acquire) & (1u << port);
        // Retirement acknowledgement never admits ordinary traffic from the old session.
        if (Failed(event.source)) return event.type == EventType::SourceReset && !pending;
        if (pending) return false;
        event.sequence = sequence_++;
        if (queue_.Push(event)) return true;
        failed_session_[port].store(event.source.session, std::memory_order_release);
        pending_faults_.fetch_or(1u << port, std::memory_order_release);
        ++lost_;
        return false;
    }

    template <typename Sink>
    size_t Drain(Sink&& sink, size_t budget = Capacity) {
        DeliverFaults(sink, 0, false);
        Event e;
        size_t consumed = 0;
        while (consumed < budget && queue_.Pop(e)) {
            ++consumed;
            if (!Failed(e.source)) sink(e);
        }
        return consumed;
    }

    template <typename Sink>
    size_t Drain(uint32_t now_us, Sink&& sink, size_t budget = Capacity) {
        return Drain([&](Event event) {
            if (!event.timestamp_valid) { event.time_us = now_us; event.timestamp_valid = true; }
            sink(event);
        }, budget);
    }

    // Scheduled input must be ordered by time within the wrapping half-period.
    template <typename Sink>
    size_t DrainDue(uint32_t now_us, Sink&& sink, size_t budget = Capacity) {
        DeliverFaults(sink, now_us, true);
        Event e;
        size_t consumed = 0;
        while (consumed < budget && queue_.Peek(e)) {
            if (!Failed(e.source) && e.timestamp_valid && !TimeReached(now_us, e.time_us)) break;
            queue_.Pop(e);
            ++consumed;
            if (!Failed(e.source)) {
                if (!e.timestamp_valid) { e.time_us = now_us; e.timestamp_valid = true; }
                sink(e);
            }
        }
        return consumed;
    }

    bool Failed(SourceId source) const {
        if (source.port >= Sources || !source.session) return true;
        const auto retired = failed_session_[source.port].load(std::memory_order_acquire);
        return retired && !SessionAfter(source.session, retired);
    }
    size_t Size() const { return queue_.Size(); }
    uint32_t Lost() const { return lost_; } // producer-owned counter

  private:
    template <typename Sink>
    void DeliverFaults(Sink& sink, uint32_t now_us, bool timestamp_valid) {
        const auto faults = pending_faults_.load(std::memory_order_acquire);
        for (size_t i = 0; i < Sources; ++i) {
            if (faults & (1u << i)) {
                auto reset = Event::System(EventType::SourceReset,
                    {static_cast<uint16_t>(i), failed_session_[i].load(std::memory_order_acquire)}, now_us);
                reset.timestamp_valid = timestamp_valid;
                sink(reset);
            }
        }
        pending_faults_.fetch_and(~faults, std::memory_order_release);
    }
    Queue<Event, Capacity> queue_;
    std::array<std::atomic<uint32_t>, Sources> failed_session_{};
    std::atomic<uint32_t> pending_faults_{0};
    uint32_t sequence_ = 0;
    uint32_t lost_ = 0;
};

} // namespace alchemy::midi
