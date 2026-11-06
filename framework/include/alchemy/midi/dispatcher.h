#pragma once

#include <array>
#include <cstddef>
#include "alchemy/midi/queue.h"
#include "alchemy/midi/types.h"

namespace alchemy::midi {

struct Observer {
    void* context = nullptr;
    void (*receive)(void*, const Event&) = nullptr;
};

struct CriticalSink {
    void* context = nullptr;
    bool (*receive)(void*, const Event&) = nullptr;
};

// Foreground owner; observers must not mutate the dispatcher during delivery.
// Ports use dense logical IDs and monotonically ordered timestamps.
template <size_t Ports = 4, size_t Capacity = 64, size_t Observers = 4>
class Dispatcher {
    static_assert(Ports > 0 && Ports < kAnyPort && Observers > 0, "Invalid dispatcher capacity");
    struct Consumer {
        void* context = nullptr;
        void (*observe)(void*, const Event&) = nullptr;
        bool (*accept)(void*, const Event&) = nullptr;
    };
    struct Port {
        Queue<Event, Capacity> queue;
        SourceId source{};
        std::array<Event, 2> resets{};
        uint8_t reset_count = 0;
        uint32_t last_us = 0;
        bool attached = false;
        bool failed = false;
        bool have_time = false;
    };
  public:
    bool Subscribe(Observer observer) {
        if (!observer.receive || observer_count_ == Observers) return false;
        for (size_t i = 0; i < observer_count_; ++i)
            if (observers_[i].context == observer.context && observers_[i].observe == observer.receive) return false;
        observers_[observer_count_++] = {observer.context, observer.receive, nullptr};
        return true;
    }
    bool Subscribe(CriticalSink sink) {
        if (!sink.receive || observer_count_ == Observers) return false;
        for (size_t i = 0; i < observer_count_; ++i)
            if (observers_[i].context == sink.context && observers_[i].accept == sink.receive) return false;
        observers_[observer_count_++] = {sink.context, nullptr, sink.receive};
        return true;
    }
    bool Failed(SourceId source) const {
        if (source.port >= Ports || !source.session) return true;
        const auto& port = ports_[source.port];
        return port.attached && (source == port.source ? port.failed : !SessionAfter(source.session, port.source.session));
    }
    // Transport sinks use Accept so a restarted session attaches before its first event.
    bool Accept(Event event) {
        if (!event.Valid() || event.source.port >= Ports) return false;
        auto& port = ports_[event.source.port];
        if ((!port.attached || port.source != event.source) && !Attach(event.source)) return false;
        if (event.type == EventType::SourceReset && port.reset_count &&
            port.resets[0].source != event.source) {
            // The transport retries this reset; discard notes from its ending session now.
            port.queue.Clear();
            return false;
        }
        return Push(event);
    }

    bool Attach(SourceId source) {
        if (source.port >= Ports || !source.session) return false;
        auto& port = ports_[source.port];
        if (port.attached && port.source == source) return !port.failed;
        if (port.attached && !SessionAfter(source.session, port.source.session)) return false;
        if (port.reset_count) {
            // Poll delivers this reset before any notes from the new session.
            if (port.reset_count != 1 || port.resets[0].source != port.source) return false;
            port.queue.Clear();
        } else if (port.attached) {
            Fault(port, port.last_us, true, port.have_time);
        }
        port.source = source;
        port.attached = true;
        port.failed = false;
        port.have_time = false;
        return true;
    }

    bool Push(Event event) {
        if (!event.Valid() || event.source.port >= Ports) return false;
        auto& port = ports_[event.source.port];
        if (!port.attached || port.source != event.source) return false;
        if (event.type == EventType::SourceReset) {
            if (port.failed) return port.reset_count == 0;
            Fault(port, event.time_us, false, event.timestamp_valid);
            return true;
        }
        if (port.failed) return false;
        if (port.have_time && static_cast<int32_t>(event.time_us - port.last_us) < 0) {
            Fault(port, event.time_us, true);
            ++lost_;
            return false;
        }
        port.last_us = event.time_us;
        port.have_time = true;
        event.sequence = enqueue_sequence_++;
        if (port.queue.Push(event)) return true;
        Fault(port, event.time_us, true);
        ++lost_;
        return false;
    }

    size_t Poll(uint32_t now_us, size_t budget = Ports * Capacity) {
        for (auto& port : ports_) {
            // Expire idle history before signed timestamp comparisons become ambiguous.
            constexpr uint32_t kIdleHistoryUs = uint32_t{1} << 30;
            if (port.have_time && now_us - port.last_us >= kIdleHistoryUs && !port.queue.Size())
                port.have_time = false;
            while (port.reset_count) {
                auto reset = port.resets[0];
                if (!reset.timestamp_valid) { reset.time_us = now_us; reset.timestamp_valid = true; }
                if (!Deliver(reset)) break;
                for (uint8_t i = 1; i < port.reset_count; ++i) port.resets[i - 1] = port.resets[i];
                --port.reset_count;
            }
        }
        size_t count = 0;
        while (count < budget) {
            size_t next = Ports;
            Event event, candidate;
            for (size_t i = 0; i < Ports; ++i) {
                if (ports_[i].reset_count || !ports_[i].queue.Peek(candidate) || !TimeReached(now_us, candidate.time_us)) continue;
                if (next == Ports || static_cast<int32_t>(candidate.time_us - event.time_us) < 0 ||
                    (candidate.time_us == event.time_us &&
                     static_cast<int32_t>(candidate.sequence - event.sequence) < 0)) {
                    event = candidate;
                    next = i;
                }
            }
            if (next == Ports) break;
            ports_[next].queue.Pop(event);
            if (!Deliver(event)) { Fault(ports_[next], event.time_us, true); ++lost_; }
            ++count;
        }
        return count;
    }

    uint32_t Lost() const { return lost_; }

  private:
    static void Fault(Port& port, uint32_t time_us, bool poison, bool timestamp_valid = true) {
        auto reset = Event::System(EventType::SourceReset, port.source, time_us);
        reset.timestamp_valid = timestamp_valid;
        if (port.reset_count && port.resets[port.reset_count - 1].source == port.source)
            port.resets[port.reset_count - 1] = reset;
        else
            port.resets[port.reset_count++] = reset;
        port.failed = poison;
        port.queue.Clear();
    }
    bool Deliver(Event event) {
        event.sequence = sequence_++;
        bool accepted = true;
        for (size_t i = 0; i < observer_count_; ++i) {
            const auto& sink = observers_[i];
            if (sink.accept) accepted = sink.accept(sink.context, event) && accepted;
            else sink.observe(sink.context, event);
        }
        return accepted;
    }
    std::array<Port, Ports> ports_{};
    std::array<Consumer, Observers> observers_{};
    size_t observer_count_ = 0;
    uint32_t sequence_ = 0, enqueue_sequence_ = 0, lost_ = 0;
};

} // namespace alchemy::midi
