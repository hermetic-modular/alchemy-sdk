#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include "alchemy/midi/types.h"

namespace alchemy::midi {

enum class ControllerKind : uint8_t {
    Cc7, Cc14, Rpn, Nrpn, RpnDelta, NrpnDelta, Program,
    Bend, ChannelPressure, PolyPressure, Reset, Disconnect
};

struct ControllerMessage {
    ControllerKind kind = ControllerKind::Cc7;
    SourceId source{};
    uint32_t time_us = 0;
    uint32_t sequence = 0;
    int32_t value = 0;
    uint16_t number = 0;
    uint8_t channel = 0;
    uint8_t resolution = 7;

    bool Valid() const {
        if (source.port == kAnyPort || !source.session || kind > ControllerKind::Disconnect) return false;
        if (kind == ControllerKind::Reset || kind == ControllerKind::Disconnect)
            return !value && !resolution && (kind == ControllerKind::Disconnect
                ? number == 0 && channel == kAnyChannel
                : (number == 0 && channel == kAnyChannel) || (number == 121 && channel < 16));
        if (channel >= 16) return false;
        switch (kind) {
            case ControllerKind::Cc7: return number < 128 && value >= 0 && value < 128 && resolution == 7;
            case ControllerKind::Cc14: return number < 32 && value >= 0 && value < 16384 && resolution == 14;
            case ControllerKind::Rpn: case ControllerKind::Nrpn:
                return number < 16383 && value >= 0 && value < 16384 && resolution == 14;
            case ControllerKind::RpnDelta: case ControllerKind::NrpnDelta:
                return number < 16383 && (value == -1 || value == 1) && resolution == 0;
            case ControllerKind::Program:
                return number < 16384 && value >= 0 && value < 128 && resolution == 7;
            case ControllerKind::Bend:
                return !number && value >= 0 && value < 16384 && resolution == 14;
            case ControllerKind::ChannelPressure:
                return !number && value >= 0 && value < 128 && resolution == 7;
            case ControllerKind::PolyPressure:
                return number < 128 && value >= 0 && value < 128 && resolution == 7;
            default: return false;
        }
    }
};

constexpr bool IsReservedController(uint8_t cc) {
    return cc == 0 || cc == 6 || cc == 32 || cc == 38
        || (cc >= 64 && cc <= 69) || cc == 88
        || (cc >= 96 && cc <= 101) || cc >= 120;
}

template <size_t Contexts = 16>
class ControllerDecoder {
    static_assert(Contexts > 0, "A controller decoder needs context storage");
    struct Context {
        bool used = false;
        SourceId source{};
        uint8_t channel = 0;
        std::array<uint8_t, 32> coarse{};
        uint8_t bank_msb = 0, bank_lsb = 0;
        uint8_t rpn_msb = 127, rpn_lsb = 127;
        uint8_t nrpn_msb = 127, nrpn_lsb = 127;
        uint8_t family = 0;
        uint8_t data_msb = 0, data_lsb = 0;
    };

  public:
    void Clear() { contexts_ = {}; }

    void ResetSource(SourceId source) {
        for (auto& c : contexts_)
            if (c.used && c.source == source) c = {};
    }

    template <typename Sink>
    bool Process(const Event& event, Sink&& sink) {
        if (!event.Valid()) return false;
        if (event.type == EventType::SourceReset) {
            ResetSource(event.source);
            Emit(event, ControllerKind::Disconnect, 0, 0, 0, sink);
            return true;
        }
        if (event.type == EventType::SystemReset) {
            ResetSource(event.source);
            Emit(event, ControllerKind::Reset, 0, 0, 0, sink);
            return true;
        }
        if (event.channel > 15) return false;
        switch (event.type) {
            case EventType::ControlChange: {
                auto* context = Find(event.source, event.channel);
                if (!context) return false;
                Cc(*context, event, sink);
                return true;
            }
            case EventType::ProgramChange: {
                auto* context = Find(event.source, event.channel);
                if (!context) return false;
                const uint16_t bank = static_cast<uint16_t>((context->bank_msb << 7) | context->bank_lsb);
                Emit(event, ControllerKind::Program, bank, event.value, 7, sink);
                return true;
            }
            case EventType::PitchBend:
                Emit(event, ControllerKind::Bend, 0, event.value, 14, sink);
                return true;
            case EventType::ChannelPressure:
                Emit(event, ControllerKind::ChannelPressure, 0, event.value, 7, sink);
                return true;
            case EventType::PolyPressure:
                Emit(event, ControllerKind::PolyPressure, event.data1, event.value, 7, sink);
                return true;
            default: return true;
        }
    }

  private:
    Context* Find(SourceId source, uint8_t channel) {
        Context* free = nullptr;
        for (auto& c : contexts_) {
            if (c.used && c.source == source && c.channel == channel) return &c;
            if (!c.used && !free) free = &c;
        }
        if (free) {
            *free = {};
            free->used = true;
            free->source = source;
            free->channel = channel;
        }
        return free;
    }

    template <typename Sink>
    static void Emit(const Event& e, ControllerKind kind, uint16_t number,
                     int32_t value, uint8_t resolution, Sink& sink) {
        const uint8_t channel = kind == ControllerKind::Disconnect ||
            (kind == ControllerKind::Reset && !number) ? kAnyChannel : e.channel;
        sink(ControllerMessage{kind, e.source, e.time_us, e.sequence,
                               value, number, channel, resolution});
    }

    static uint16_t Address(const Context& c) {
        return c.family == 1
            ? static_cast<uint16_t>((c.rpn_msb << 7) | c.rpn_lsb)
            : static_cast<uint16_t>((c.nrpn_msb << 7) | c.nrpn_lsb);
    }

    template <typename Sink>
    static void Cc(Context& c, const Event& e, Sink& sink) {
        const auto cc = e.data1;
        const auto value = static_cast<uint8_t>(e.value);
        Emit(e, ControllerKind::Cc7, cc, value, 7, sink);
        if (cc == 0) c.bank_msb = value;
        if (cc == 32) c.bank_lsb = value;
        if (cc < 32) {
            c.coarse[cc] = value;
            Emit(e, ControllerKind::Cc14, cc, value << 7, 14, sink);
        } else if (cc < 64) {
            const uint8_t index = cc - 32;
            Emit(e, ControllerKind::Cc14, index,
                 (c.coarse[index] << 7) | value, 14, sink);
        }
        if (cc >= 98 && cc <= 101) {
            const uint8_t old_family = c.family;
            const uint16_t old_address = Address(c);
            if (cc == 101) { c.family = 1; c.rpn_msb = value; }
            if (cc == 100) { c.family = 1; c.rpn_lsb = value; }
            if (cc == 99)  { c.family = 2; c.nrpn_msb = value; }
            if (cc == 98)  { c.family = 2; c.nrpn_lsb = value; }
            if (old_family != c.family || old_address != Address(c)) {
                c.data_msb = c.data_lsb = 0;
                c.coarse[6] = 0;
            }
            return;
        }
        if (cc == 121) {
            c.family = 0;
            c.rpn_msb = c.rpn_lsb = c.nrpn_msb = c.nrpn_lsb = 127;
            c.data_msb = c.data_lsb = 0;
            c.coarse[6] = 0;
            // Bank/program selection survives controller reset.
            c.coarse[1] = 0;
            c.coarse[11] = 127;
            Emit(e, ControllerKind::Reset, 121, 0, 0, sink);
            return;
        }
        if (c.family == 0 || Address(c) == 16383) return;
        const bool rpn = c.family == 1;
        if (cc == 6 || cc == 38) {
            if (cc == 6) { c.data_msb = value; c.data_lsb = 0; }
            else c.data_lsb = value;
            Emit(e, rpn ? ControllerKind::Rpn : ControllerKind::Nrpn,
                 Address(c), (c.data_msb << 7) | c.data_lsb, 14, sink);
        } else if (cc == 96 || cc == 97) {
            Emit(e, rpn ? ControllerKind::RpnDelta : ControllerKind::NrpnDelta,
                 Address(c), cc == 96 ? 1 : -1, 0, sink);
        }
    }

    std::array<Context, Contexts> contexts_{};
};

} // namespace alchemy::midi
