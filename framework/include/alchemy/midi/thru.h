#pragma once

#include <array>
#include <cstddef>
#include "alchemy/midi/types.h"

namespace alchemy::midi {

enum class MessageClass : uint8_t { Notes, Controllers, Expression, Program, SystemCommon, Realtime };
constexpr uint32_t ClassBit(MessageClass type) { return uint32_t{1} << static_cast<uint8_t>(type); }

struct ThruRoute {
    Listen listen{};
    uint16_t output_port = kAnyPort;
    uint32_t classes = ClassBit(MessageClass::Notes);
    bool allow_same_port = false;
};

template <size_t Capacity = 4>
class Thru {
  public:
    bool Add(ThruRoute route) {
        if (!route.listen.channel_mask || route.output_port == kAnyPort || !route.classes
            || (route.classes & ~uint32_t{0x3f}) || size_ == Capacity) return false;
        constexpr auto system_classes = ClassBit(MessageClass::SystemCommon) | ClassBit(MessageClass::Realtime);
        for (size_t i = 0; i < size_; ++i) {
            const auto& existing = routes_[i];
            const auto common_classes = existing.classes & route.classes;
            const auto common_port = existing.listen.port == kAnyPort ? route.listen.port : existing.listen.port;
            if (common_port == route.output_port && (!existing.allow_same_port || !route.allow_same_port)) continue;
            if (existing.output_port == route.output_port && common_classes
                && (existing.listen.port == kAnyPort || route.listen.port == kAnyPort
                    || existing.listen.port == route.listen.port)
                && ((common_classes & system_classes)
                    || (existing.listen.channel_mask & route.listen.channel_mask))) return false;
        }
        routes_[size_++] = route;
        return true;
    }
    void Clear() { size_ = 0; }

    template <typename Sink>
    size_t Process(const Event& event, Sink&& sink) const {
        if (!event.Valid()) return 0;
        uint32_t bit = 0;
        switch (event.type) {
        case EventType::NoteOn: case EventType::NoteOff: bit = ClassBit(MessageClass::Notes); break;
        case EventType::ControlChange: bit = ClassBit(MessageClass::Controllers); break;
        case EventType::PolyPressure: case EventType::ChannelPressure: case EventType::PitchBend:
            bit = ClassBit(MessageClass::Expression); break;
        case EventType::ProgramChange: bit = ClassBit(MessageClass::Program); break;
        case EventType::TimeCode: case EventType::SongPosition: case EventType::SongSelect: case EventType::TuneRequest:
            bit = ClassBit(MessageClass::SystemCommon); break;
        case EventType::TimingClock: case EventType::Start: case EventType::Continue: case EventType::Stop:
        case EventType::ActiveSensing: case EventType::SystemReset: bit = ClassBit(MessageClass::Realtime); break;
        default: return 0;
        }
        size_t count = 0;
        for (size_t i = 0; i < size_; ++i) {
            const auto& route = routes_[i];
            if (!(route.classes & bit) || !route.listen.Matches(event)
                || (!route.allow_same_port && route.output_port == event.source.port)) continue;
            if (sink(route.output_port, event)) ++count;
        }
        return count;
    }

  private:
    std::array<ThruRoute, Capacity> routes_{};
    size_t size_ = 0;
};

} // namespace alchemy::midi
