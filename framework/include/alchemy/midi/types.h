#pragma once

#include <cstdint>

namespace alchemy::midi {

using ParamId = uint32_t;
constexpr uint16_t kAnyPort = 0xffffu;
constexpr uint8_t kAnyChannel = 0xffu;

struct SourceId {
    uint16_t port = 0;
    // Nonzero, monotonically advancing generation for a physical port (wrap skips zero).
    uint32_t session = 1;
    constexpr bool operator==(SourceId rhs) const {
        return port == rhs.port && session == rhs.session;
    }
    constexpr bool operator!=(SourceId rhs) const { return !(*this == rhs); }
};

constexpr bool SessionAfter(uint32_t candidate, uint32_t previous) {
    return candidate && static_cast<int32_t>(candidate - previous) > 0;
}

enum class EventType : uint8_t {
    NoteOn, NoteOff, PolyPressure, ControlChange, ProgramChange,
    ChannelPressure, PitchBend, TimeCode, SongPosition, SongSelect,
    TuneRequest, TimingClock, Start, Continue, Stop, ActiveSensing,
    SystemReset,
    SourceReset,  // Retire musical state for this source only.
    SysEx
};

enum class NoteOffOrigin : uint8_t { Explicit, ZeroVelocityNoteOn, Unknown };

struct Event {
    EventType type = EventType::NoteOff;
    SourceId source{};
    uint32_t time_us = 0;
    uint32_t sequence = 0;
    uint16_t value = 0;
    uint8_t channel = 0;
    uint8_t data1 = 0;
    uint8_t resolution = 7;
    NoteOffOrigin note_off_origin = NoteOffOrigin::Explicit;
    bool timestamp_valid = true;

    static constexpr Event NoteOn(SourceId s, uint8_t ch, uint8_t key,
                                  uint8_t velocity, uint32_t time = 0) {
        auto event = Make(velocity ? EventType::NoteOn : EventType::NoteOff,
                          s, ch, key, velocity, time);
        if (!velocity) event.note_off_origin = NoteOffOrigin::ZeroVelocityNoteOn;
        return event;
    }
    static constexpr Event NoteOff(SourceId s, uint8_t ch, uint8_t key,
                                   uint8_t velocity = 0, uint32_t time = 0) {
        return Make(EventType::NoteOff, s, ch, key, velocity, time);
    }
    static constexpr Event Cc(SourceId s, uint8_t ch, uint8_t cc,
                              uint8_t value, uint32_t time = 0) {
        return Make(EventType::ControlChange, s, ch, cc, value, time);
    }
    static constexpr Event Bend(SourceId s, uint8_t ch, uint16_t value,
                                uint32_t time = 0) {
        auto e = Make(EventType::PitchBend, s, ch, 0, value, time);
        e.resolution = 14;
        return e;
    }
    static constexpr Event System(EventType type, SourceId s,
                                   uint32_t time = 0, uint16_t value = 0) {
        return Make(type, s, 0, 0, value, time);
    }
    constexpr bool IsChannel() const {
        return type <= EventType::PitchBend;
    }
    constexpr bool Valid() const {
        if (source.port == kAnyPort || !source.session ||
            type > EventType::SysEx || note_off_origin > NoteOffOrigin::Unknown
            || (!timestamp_valid && type != EventType::SourceReset)
            || (IsChannel() && channel >= 16)) return false;
        switch (type) {
        case EventType::NoteOn: case EventType::NoteOff:
            return !(type == EventType::NoteOff && note_off_origin == NoteOffOrigin::ZeroVelocityNoteOn && value)
                && data1 < 128 && ((resolution == 7 && value < 128)
                || (resolution == 14 && value < 16384));
        case EventType::PolyPressure: case EventType::ControlChange:
            return resolution == 7 && data1 < 128 && value < 128;
        case EventType::ProgramChange: case EventType::ChannelPressure:
        case EventType::SongSelect: return value < 128;
        case EventType::PitchBend: case EventType::SongPosition:
            return value < 16384;
        case EventType::TimeCode: return value < 128;
        default: return true;
        }
    }

  private:
    static constexpr Event Make(EventType t, SourceId s, uint8_t ch,
                                 uint8_t first, uint16_t v, uint32_t time) {
        Event e;
        e.type = t; e.source = s; e.channel = ch; e.data1 = first;
        e.value = v; e.time_us = time;
        return e;
    }
};

struct Listen {
    uint16_t channel_mask = 1;
    uint16_t port = kAnyPort;
    static constexpr Listen Channel(uint8_t one_based, uint16_t source_port = kAnyPort) {
        return {static_cast<uint16_t>(one_based >= 1 && one_based <= 16
                                      ? 1u << (one_based - 1u) : 0u), source_port};
    }
    static constexpr Listen Omni(uint16_t source_port = kAnyPort) {
        return {0xffffu, source_port};
    }
    constexpr bool Matches(const Event& e) const {
        return (port == kAnyPort || port == e.source.port) &&
               (!e.IsChannel() || (e.channel < 16 &&
                 (channel_mask & (1u << e.channel)) != 0));
    }
};

constexpr bool TimeReached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}
constexpr float Bipolar7(uint16_t value) {
    return value < 64 ? (static_cast<float>(value) - 64.f) / 64.f
                      : (static_cast<float>(value) - 64.f) / 63.f;
}
constexpr float Bipolar14(uint16_t value) {
    return value < 8192 ? (static_cast<float>(value) - 8192.f) / 8192.f
                        : (static_cast<float>(value) - 8192.f) / 8191.f;
}

} // namespace alchemy::midi
