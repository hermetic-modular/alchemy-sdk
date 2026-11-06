#pragma once
#include "alchemy/midi/raw_input.h"
#include "alchemy/midi/wire.h"

namespace alchemy::midi {
struct InputActivity {
    void* context = nullptr;
    void (*receive)(void*, SourceId, uint32_t) = nullptr;
};

struct TransportCapabilities {
    bool receive = true;
    bool transmit = false;
    bool observable_tx_acceptance = false;
    bool realtime_transmit = false;
    bool system_common_transmit = false;
    bool nonblocking_transmit = true;
    bool streaming_sysex = true;
    bool multiple_cables = false;
    bool parser_conformant = true;
    bool zero_velocity_note_on_provenance = true;
    bool high_resolution_release_velocity = true;
};

// Receive is the ISR producer; Poll is the foreground consumer. Restart requires a quiesced producer.
template<size_t ByteCapacity = 256> class Receiver {
  public:
    bool Init(SourceId source, uint32_t tick_frequency_hz) {
        if (initialized_ || source.port == kAnyPort || !source.session || !tick_frequency_hz) return false;
        frequency_ = tick_frequency_hz;
        Begin(source);
        return true;
    }
    bool Restart(SourceId source) {
        if (!initialized_ || source.port != source_.port || !SessionAfter(source.session, source_.session)) return false;
        Begin(source);
        return true;
    }
    void Receive(const uint8_t* bytes, size_t size, uint32_t tick) { raw_.Receive(bytes, size, tick); }
    void MarkLost() { raw_.MarkLost(); }
    // Poll at least every two seconds so buffered arrival ticks remain unambiguous.
    template<class Sink> size_t Poll(TickTimebase::Sample now, Sink&& sink,
                                      size_t byte_budget = ByteCapacity) {
        if (!initialized_) return 0;
        const uint32_t now_tick = now.raw_tick, now_us = now.time_us;
        const SourceId processing_source = source_;
        const bool valid_timebase = now.Valid() && now.frequency_hz == frequency_;
        if (!valid_timebase) raw_.MarkLost();
        if (have_poll_ && TimeReached(now_us, last_poll_us_) && now_us - last_poll_us_ > 2000000u && raw_.Size()) raw_.MarkLost();
        last_poll_us_ = now_us; have_poll_ = true;
        if (raw_.Lost()) {
            if (!fault_reported_) {
                auto e = Event::System(EventType::SourceReset, source_, now_us);
                e.timestamp_valid = valid_timebase;
                e.sequence = ++sequence_;
                if (sink(e) && source_ == processing_source && raw_.Lost()) {
                    decoder_.Abort(now_us);
                    if (source_ == processing_source && raw_.Lost()) { raw_.DiscardLost(); fault_reported_ = true; }
                }
            }
            return 0;
        }
        size_t count = 0;
        StampedByte byte;
        while (count < byte_budget && source_ == processing_source && !raw_.Lost() && raw_.Peek(byte)) {
            if (static_cast<int32_t>(now_tick - byte.tick) < 0) {
                if (!deferred_ || deferred_tick_ != byte.tick) {
                    deferred_ = true; deferred_tick_ = byte.tick; deferred_since_us_ = now_us;
                } else if (TimeReached(now_us, deferred_since_us_) && now_us - deferred_since_us_ > 2000000u) {
                    raw_.MarkLost();
                }
                break;
            }
            deferred_ = false;
            if (!raw_.Pop(byte)) break;
            ++count;
            uint32_t stamp = 0;
            if (!ArrivalTime(byte.tick, now, stamp)) { raw_.MarkLost(); break; }
            if (raw_.Lost()) break;
            last_activity_us_ = stamp; have_activity_ = true;
            if (activity_.receive) activity_.receive(activity_.context, source_, stamp);
            if (source_ != processing_source || raw_.Lost()) break;
            Event event;
            const bool decoded = decoder_.Feed(byte.byte, stamp, event);
            if (source_ != processing_source || raw_.Lost()) break;
            if (!decoded) continue;
            event.sequence = ++sequence_;
            if (raw_.Lost()) break;
            const bool accepted = sink(event);
            if (source_ != processing_source) break;
            if (!accepted) { raw_.MarkLost(); break; }
        }
        return count;
    }
    SourceId Source() const { return source_; }
    bool Faulted() const { return raw_.Lost(); }
    bool ResetReported() const { return fault_reported_; }
    void SetActivity(InputActivity observer) { activity_ = observer; }
    void SetSysExSink(SysExSink observer) { decoder_.SetSysExSink(observer); }
    uint32_t Unsupported() const { return decoder_.Ignored(); }
    bool HasActivity() const { return have_activity_; }
    uint32_t LastActivityUs() const { return last_activity_us_; }
  private:
    void Begin(SourceId source) {
        source_ = source; sequence_ = 0;
        fault_reported_ = false; have_activity_ = false; deferred_ = false;
        have_poll_ = false;
        raw_.Restart(); decoder_.Init(source); initialized_ = true;
    }
    RawInput<ByteCapacity> raw_;
    StreamDecoder decoder_;
    SourceId source_{};
    uint32_t frequency_ = 0, sequence_ = 0;
    bool initialized_ = false, fault_reported_ = false, have_activity_ = false;
    uint32_t last_activity_us_ = 0;
    uint32_t deferred_tick_ = 0, deferred_since_us_ = 0;
    uint32_t last_poll_us_ = 0;
    bool have_poll_ = false;
    bool deferred_ = false;
    InputActivity activity_{};
};
}
