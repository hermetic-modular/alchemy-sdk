#pragma once
#include <cmath>
#include "alchemy/midi/types.h"
#include "alchemy/control/clock_follower.h"
#include "alchemy/control/musical_clock.h"

namespace alchemy::midi {
enum class TransportPolicy : uint8_t { Follow, ClockOnly };

// This adapter owns MusicalClock::Tick. Drain MIDI before Poll using the same microsecond epoch.
class ClockAdapter {
  public:
    ClockAdapter(ClockFollower& follower, MusicalClock& clock)
        : follower_(follower), clock_(clock) {}
    bool SelectSource(SourceId source) {
        if (!source.session || source.port == kAnyPort || !clock_.Ppqn() || clock_.Ppqn() % 24) return false;
        if (enabled_ && source == source_) return true;
        if (have_source_ && source.port == source_.port && source.session != source_.session &&
            !SessionAfter(source.session, source_.session)) return false;
        source_ = source; have_source_ = true;
        enabled_ = true;
        reanchor_ = false;
        armed_ = Armed::None;
        have_pulse_anchor_ = false; stream_ = StreamState::Cold;
        follower_.SetAutoStart(false);
        follower_.Enable(24);
        return true;
    }
    void Disable() {
        enabled_ = false; reanchor_ = false; armed_ = Armed::None;
        have_pulse_anchor_ = false; follower_.Disable();
    }
    void SetTransportPolicy(TransportPolicy policy) {
        if (policy != TransportPolicy::Follow && policy != TransportPolicy::ClockOnly) return;
        policy_ = policy;
        if (policy != TransportPolicy::Follow) armed_ = Armed::None;
    }
    void SetLossPolicy(ClockLossPolicy policy) { follower_.SetLossPolicy(policy); }
    bool Process(const Event& e) {
        if (!e.Valid() || !enabled_ || e.source != source_) return false;
        if (e.type == EventType::TimingClock) {
            if (stream_ == StreamState::Lost && TimeReached(e.time_us, loss_time_us_)) {
                // Confirmed loss retires the old pulse count; late input alone does not.
                Advance(e.time_us);
                have_pulse_anchor_ = false;
                stream_ = StreamState::Reacquiring;
            }
            TickTransport(e.time_us);
            if (armed_ != Armed::None || (policy_ == TransportPolicy::ClockOnly && !clock_.Running())) {
                if (armed_ == Armed::Start) clock_.Reset();
                clock_.Start(); reanchor_ = true; armed_ = Armed::None; have_pulse_anchor_ = false;
                TickTransport(e.time_us);
            }
            if (clock_.Running()) {
                if (!have_pulse_anchor_) {
                    pulse_tick_ = clock_.MasterTick(); pulse_frac_ = clock_.FracTick();
                    have_pulse_anchor_ = true;
                } else pulse_tick_ += clock_.Ppqn() / 24;
                pulse_time_us_ = e.time_us;
                if (CanSynchronize()) clock_.Synchronize(pulse_tick_, last_tick_time_, pulse_frac_);
                pending_events_ |= clock_.Events();
            } else have_pulse_anchor_ = false;
            follower_.OnPulse(e.time_us);
            follower_.Update(e.time_us);
            if (follower_.Locked()) {
                stream_ = StreamState::Tracking;
                if (clock_.Running() && have_pulse_anchor_)
                    clock_.Synchronize(pulse_tick_, last_tick_time_, pulse_frac_);
                pending_events_ |= clock_.Events();
                follower_.OnSeek();
            } else if (stream_ == StreamState::Cold && clock_.Running() && follower_.Bpm() > 0.f) {
                // The second cold pulse supplies a period before lock.
                const float bpm = follower_.Bpm() < kClockBpmMax ? follower_.Bpm() : kClockBpmMax;
                clock_.SetTicksPerUs(double(bpm) * clock_.Ppqn() / 60000000.0);
            }
            return true;
        }
        if (e.type == EventType::SourceReset || e.type == EventType::SystemReset) {
            // Untimed resets use the last sample, or wait for the first Poll.
            if (e.timestamp_valid) Advance(e.time_us);
            if (e.type == EventType::SystemReset) clock_.Reset();
            clock_.Stop(); armed_ = Armed::None; have_pulse_anchor_ = false;
            if (e.timestamp_valid || have_tick_time_)
                TickTransport(e.timestamp_valid ? e.time_us : last_tick_time_);
            follower_.Enable(24); reanchor_ = false; stream_ = StreamState::Cold;
            return true;
        }
        if (policy_ != TransportPolicy::Follow) return false;
        switch (e.type) {
            case EventType::Start:
                Advance(e.time_us);
                armed_ = Armed::Start; have_pulse_anchor_ = false;
                if (clock_.Running()) { clock_.Stop(); TickTransport(e.time_us); }
                return true;
            case EventType::Continue:
                Advance(e.time_us);
                if (!clock_.Running()) { armed_ = Armed::Continue; have_pulse_anchor_ = false; }
                return true;
            case EventType::Stop:
                Advance(e.time_us);
                armed_ = Armed::None; have_pulse_anchor_ = false;
                clock_.Stop(); TickTransport(e.time_us); return true;
            case EventType::SongPosition:
                Advance(e.time_us);
                clock_.Seek(uint64_t(e.value) * (clock_.Ppqn() / 4));
                reanchor_ = true; have_pulse_anchor_ = false;
                return true;
            default: return false;
        }
    }
    bool Process(Event e, uint32_t now_us) {
        if (e.type == EventType::SourceReset && !e.timestamp_valid) {
            e.time_us = now_us;
            e.timestamp_valid = true;
        }
        return Process(e);
    }
    void Poll(uint32_t now_us) {
        // Preserve new intervals during reacquisition, even at a much slower tempo.
        if (enabled_ && (stream_ == StreamState::Cold || stream_ == StreamState::Tracking))
            follower_.Update(now_us);
        Advance(now_us);
        clock_.OrPendingEvent(pending_events_);
        pending_events_ = 0;
        clock_.Tick(TickTime(now_us));
    }
    bool Enabled() const { return enabled_; }
    SourceId Source() const { return source_; }
  private:
    enum class Armed : uint8_t { None, Start, Continue };
    enum class StreamState : uint8_t { Cold, Tracking, Lost, Reacquiring };
    uint32_t TickTime(uint32_t time_us) {
        if (!have_tick_time_ || TimeReached(time_us, last_tick_time_)) {
            last_tick_time_ = time_us; have_tick_time_ = true;
        }
        return last_tick_time_;
    }
    void TickTransport(uint32_t time_us) {
        const double rate = clock_.TicksPerUs();
        clock_.SetTicksPerUs(0.0);
        clock_.Tick(TickTime(time_us));
        clock_.SetTicksPerUs(rate);
        CaptureEvents();
    }
    void Advance(uint32_t time_us) {
        const uint32_t safe_time = TickTime(time_us);
        const bool anchored = enabled_ && have_pulse_anchor_ && clock_.Running() && CanSynchronize();
        const double rate = clock_.TicksPerUs();
        if (anchored) clock_.SetTicksPerUs(0.0);
        clock_.Tick(safe_time);
        if (anchored) clock_.SetTicksPerUs(rate);
        // Project from the source anchor; hold an ahead phase until it catches up.
        if (enabled_ && have_pulse_anchor_ && clock_.Running() && CanSynchronize()
            && TimeReached(safe_time, pulse_time_us_)) {
            const double travel = pulse_frac_ + clock_.TicksPerUs() * double(safe_time - pulse_time_us_);
            if (travel >= 0.0 && travel < 0x1p64) {
                const uint64_t whole = uint64_t(travel);
                clock_.Synchronize(pulse_tick_ + whole, safe_time, travel - double(whole));
            }
        }
        CaptureEvents();
    }
    bool CanSynchronize() const {
        // Freeze continues holding position during warm lock acquisition.
        return stream_ == StreamState::Cold || follower_.Locked() || clock_.TicksPerUs() > 0.0;
    }
    void CaptureEvents() {
        pending_events_ |= clock_.Events();
        if ((clock_.Events() & CLK_EV::EXT_LOST) && stream_ != StreamState::Lost
            && stream_ != StreamState::Reacquiring) {
            stream_ = StreamState::Lost;
            loss_time_us_ = last_tick_time_;
        }
        if (reanchor_) { follower_.OnSeek(); reanchor_ = false; }
    }
    ClockFollower& follower_;
    MusicalClock& clock_;
    SourceId source_{};
    TransportPolicy policy_ = TransportPolicy::Follow;
    bool enabled_ = false, have_source_ = false;
    bool reanchor_ = false;
    StreamState stream_ = StreamState::Cold;
    bool have_tick_time_ = false, have_pulse_anchor_ = false;
    uint32_t last_tick_time_ = 0, pending_events_ = 0, pulse_time_us_ = 0, loss_time_us_ = 0;
    uint64_t pulse_tick_ = 0;
    double pulse_frac_ = 0.0;
    Armed armed_ = Armed::None;
};
}
