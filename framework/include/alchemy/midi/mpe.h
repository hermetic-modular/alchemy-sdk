#pragma once

#include "alchemy/midi/types.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace alchemy::midi {

struct NoteExpression {
    float bend_semitones = 0.f;
    float pressure = 0.f;
    float timbre = 64.f / 127.f;
    float zone_bend_semitones = 0.f;
    float zone_pressure = 0.f;
    float zone_timbre = 64.f / 127.f;
    float key_pressure = 0.f;
    float TotalBend() const { return bend_semitones + zone_bend_semitones; }
};

enum class MpeZone : uint8_t { Lower, Upper };
enum class MpeRole : uint8_t { None, Manager, Member };

struct MpeUpdate {
    uint16_t reset_channels = 0;
    uint16_t expression_channels = 0;
    bool capacity_exceeded = false;
};

template <std::size_t Sources>
class MpeState {
  public:
    MpeUpdate Configure(SourceId source, MpeZone zone, uint8_t members) {
        MpeUpdate result;
        if (source.port == kAnyPort || !source.session || zone > MpeZone::Upper || members > 15)
            return result;
        auto* state = Get(source, true);
        if (!state) { result.capacity_exceeded = true; return result; }
        const uint8_t manager = zone == MpeZone::Lower ? 0 : 15;
        const auto selector = state->channels[manager];
        std::array<uint8_t, 16> previous_managers{};
        for (uint8_t ch = 0; ch < 16; ++ch) previous_managers[ch] = Manager(*state, ch);
        if (zone == MpeZone::Lower) {
            state->lower = members;
            if (members && state->upper)
                state->upper = members >= 14 ? 0 : Min(state->upper, 14 - members);
        } else {
            state->upper = members;
            if (members && state->lower)
                state->lower = members >= 14 ? 0 : Min(state->lower, 14 - members);
        }
        for (uint8_t ch = 0; ch < 16; ++ch) {
            if (previous_managers[ch] != Manager(*state, ch)) {
                result.reset_channels |= Bit(ch);
                state->channels[ch] = Channel{};
            }
            if (members && Manager(*state, ch) == manager) {
                state->channels[ch].range = ch == manager ? 2.f : 48.f;
                state->channels[ch].range_semitones = ch == manager ? 2 : 48;
                state->channels[ch].range_cents = 0;
            }
        }
        RestoreSelection(source, manager, selector.rpn_msb, selector.rpn_lsb, selector.rpn);
        result.expression_channels = ZoneMask(*state, manager);
        return result;
    }

    MpeUpdate Process(const Event& e) {
        MpeUpdate result;
        if (!e.Valid()) return result;
        if (e.type == EventType::SourceReset) { Reset(e.source); return result; }
        if (e.type == EventType::SystemReset) { Reset(); return result; }
        if (!e.IsChannel()) return result;
        if (e.type != EventType::PitchBend && e.type != EventType::ChannelPressure &&
            e.type != EventType::ControlChange) return result;
        if (e.type == EventType::ControlChange && e.data1 != 74 && e.data1 != 101 &&
            e.data1 != 100 && e.data1 != 99 && e.data1 != 98 && e.data1 != 6 &&
            e.data1 != 38 && e.data1 != 121) return result;
        auto* state = Get(e.source, !(e.type == EventType::ControlChange && e.data1 == 121));
        if (!state && e.type == EventType::ControlChange && e.data1 == 121) return result;
        if (!state) { result.capacity_exceeded = true; return result; }
        auto& channel = state->channels[e.channel];
        const auto manager = Manager(*state, e.channel);
        switch (e.type) {
        case EventType::PitchBend: channel.bend = e.value; break;
        case EventType::ChannelPressure: channel.pressure = static_cast<uint8_t>(e.value); break;
        case EventType::ControlChange:
            if (e.data1 == 74) channel.timbre = static_cast<uint8_t>(e.value);
            else if (e.data1 == 101) { channel.rpn_msb = static_cast<uint8_t>(e.value); channel.rpn = true; return result; }
            else if (e.data1 == 100) { channel.rpn_lsb = static_cast<uint8_t>(e.value); channel.rpn = true; return result; }
            else if (e.data1 == 99 || e.data1 == 98) { channel.rpn = false; return result; }
            else if (e.data1 == 6 || e.data1 == 38) {
                if (!channel.rpn || channel.rpn_msb != 0) return result;
                if (channel.rpn_lsb == 6) {
                    if (e.data1 != 6 || (e.channel != 0 && e.channel != 15) || e.value > 15)
                        return result;
                    return Configure(e.source, e.channel == 0 ? MpeZone::Lower : MpeZone::Upper,
                                     static_cast<uint8_t>(e.value));
                }
                if (channel.rpn_lsb != 0) return result;
                if (e.data1 == 6) {
                    channel.range_semitones = static_cast<uint8_t>(e.value);
                    channel.range_cents = 0;
                }
                else channel.range_cents = static_cast<uint8_t>(e.value);
                float range = channel.range_semitones + channel.range_cents / 100.f;
                channel.range = range;
                if (manager != kAnyChannel && manager != e.channel) {
                    for (uint8_t ch = 0; ch < 16; ++ch) {
                        if (ch != manager && Manager(*state, ch) == manager) {
                            state->channels[ch].range = range;
                            state->channels[ch].range_semitones = channel.range_semitones;
                            state->channels[ch].range_cents = channel.range_cents;
                        }
                    }
                    result.expression_channels = ZoneMask(*state, manager) & ~Bit(manager);
                    return result;
                }
            } else if (e.data1 == 121) {
                if (manager != kAnyChannel && manager != e.channel) return result;
                const auto scope = manager == kAnyChannel ? Bit(e.channel) : ZoneMask(*state, manager);
                for (uint8_t ch = 0; ch < 16; ++ch) {
                    if (scope & Bit(ch)) {
                        auto& c = state->channels[ch];
                        c.bend = 8192; c.pressure = 0;
                        if (manager != kAnyChannel) c.timbre = 64;
                        c.rpn_msb = c.rpn_lsb = 127; c.rpn = true;
                    }
                }
                result.expression_channels = scope;
                return result;
            } else return result;
            break;
        default: return result;
        }
        result.expression_channels = manager == e.channel ? ZoneMask(*state, manager) : Bit(e.channel);
        return result;
    }

    MpeRole Role(SourceId source, uint8_t channel) const {
        const auto* state = Find(source);
        if (!state || channel >= 16) return MpeRole::None;
        const auto manager = Manager(*state, channel);
        return manager == kAnyChannel ? MpeRole::None
             : manager == channel ? MpeRole::Manager : MpeRole::Member;
    }

    uint16_t Scope(SourceId source, uint8_t channel) const {
        const auto* state = Find(source);
        if (!state || channel >= 16) return 0;
        const auto manager = Manager(*state, channel);
        return manager == channel ? ZoneMask(*state, manager) : Bit(channel);
    }

    uint8_t ManagerChannel(SourceId source, uint8_t channel) const {
        const auto* state = Find(source);
        return state && channel < 16 ? Manager(*state, channel) : kAnyChannel;
    }

    NoteExpression Expression(SourceId source, uint8_t channel) const {
        NoteExpression result;
        const auto* state = Find(source);
        if (!state || channel >= 16) return result;
        const auto& c = state->channels[channel];
        result.bend_semitones = Bipolar14(c.bend) * c.range;
        result.pressure = c.pressure / 127.f;
        result.timbre = c.timbre / 127.f;
        const auto manager = Manager(*state, channel);
        if (manager != kAnyChannel && manager != channel) {
            const auto& z = state->channels[manager];
            result.zone_bend_semitones = Bipolar14(z.bend) * z.range;
            result.zone_pressure = z.pressure / 127.f;
            result.zone_timbre = z.timbre / 127.f;
        }
        return result;
    }

    void Reset(SourceId source) {
        for (auto& state : sources_) if (state.used && state.source == source) state = State{};
    }
    void Reset() { sources_ = {}; }

  private:
    template <std::size_t, std::size_t, std::size_t, std::size_t> friend class NotePart;
    struct Channel {
        uint16_t bend = 8192;
        uint8_t pressure = 0, timbre = 64;
        uint8_t rpn_msb = 127, rpn_lsb = 127;
        uint8_t range_semitones = 2, range_cents = 0;
        bool rpn = true;
        float range = 2.f;
    };
    struct State {
        SourceId source{};
        bool used = false;
        uint8_t lower = 0, upper = 0;
        std::array<Channel, 16> channels{};
    };
    static uint16_t Bit(uint8_t ch) { return static_cast<uint16_t>(1u << ch); }
    static uint8_t Min(uint8_t a, uint8_t b) { return a < b ? a : b; }
    static uint8_t Manager(const State& s, uint8_t ch) {
        if (s.lower && ch <= s.lower) return 0;
        if (s.upper && ch >= 15 - s.upper) return 15;
        return kAnyChannel;
    }
    static uint16_t ZoneMask(const State& s, uint8_t manager) {
        uint16_t mask = 0;
        if (manager == kAnyChannel) return mask;
        for (uint8_t ch = 0; ch < 16; ++ch) if (Manager(s, ch) == manager) mask |= Bit(ch);
        return mask;
    }
    State* Get(SourceId source, bool create) {
        for (auto& state : sources_) if (state.used && state.source == source) return &state;
        if (create) for (auto& state : sources_) if (!state.used) {
            state = State{}; state.used = true; state.source = source; return &state;
        }
        return nullptr;
    }
    void RestoreSelection(SourceId source, uint8_t channel, uint8_t msb, uint8_t lsb, bool rpn) {
        if (auto* state = Get(source, false)) {
            auto& c = state->channels[channel];
            c.rpn_msb = msb; c.rpn_lsb = lsb; c.rpn = rpn;
        }
    }
    const State* Find(SourceId source) const {
        for (const auto& state : sources_) if (state.used && state.source == source) return &state;
        return nullptr;
    }
    std::array<State, Sources> sources_{};
};

} // namespace alchemy::midi
