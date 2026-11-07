#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include "alchemy/midi/controller.h"

namespace alchemy::midi {

using RouteId = uint32_t;
enum class RouteOp : uint8_t { EditBase, RelativeBase, AddOffset, Multiply, Overlay, Select, Trigger };
enum class Reduction : uint8_t { LastEvent, Sum, Maximum, Product };
enum class RelativeEncoding : uint8_t { TwosComplement, BinaryOffset, SignedBit, SignedBit2 };
enum class TakeoverPolicy : uint8_t { Immediate, Pickup, Scale };
enum class Curve : uint8_t { Linear, Smoothstep, Power };
enum class TriggerEdge : uint8_t { Rising, Falling, Momentary, EveryEvent };

struct Route {
    RouteId id = 0;
    ParamId target = 0;
    ControllerKind kind = ControllerKind::Cc7;
    uint16_t number = 0;
    Listen listen{};
    uint32_t session = 0;
    RouteOp operation = RouteOp::EditBase;
    Reduction reduction = Reduction::LastEvent;
    RelativeEncoding encoding = RelativeEncoding::TwosComplement;
    TriggerEdge trigger = TriggerEdge::Rising;
    float minimum = 0.f;
    float maximum = 1.f;
    float step = 1.f / 127.f;
    uint16_t center = 0xffff;
    bool bipolar = false;
    bool advanced = false;
    Curve curve = Curve::Linear;
    float exponent = 1.f;
    TakeoverPolicy takeover = TakeoverPolicy::Immediate;
    float pickup_tolerance = 1.f / 127.f;

    bool Valid() const {
        if (!id || !target || !listen.channel_mask || kind > ControllerKind::PolyPressure
            || operation > RouteOp::Trigger || reduction > Reduction::Product
            || encoding > RelativeEncoding::SignedBit2 || trigger > TriggerEdge::EveryEvent
            || !std::isfinite(minimum) || !std::isfinite(maximum)
            || !std::isfinite(step) || step < 0.f || curve > Curve::Power
            || takeover > TakeoverPolicy::Scale || !std::isfinite(exponent) || exponent <= 0.f
            || !std::isfinite(pickup_tolerance) || pickup_tolerance < 0.f || pickup_tolerance > 1.f) return false;
        if (operation == RouteOp::RelativeBase && kind == ControllerKind::Cc7
            && step > std::numeric_limits<float>::max() / 64.f) return false;
        if (takeover != TakeoverPolicy::Immediate && (operation != RouteOp::EditBase || minimum == maximum)) return false;
        if ((kind == ControllerKind::Cc7 || kind == ControllerKind::Cc14)
            && (number > (kind == ControllerKind::Cc14 ? 31u : 127u)
                || (!advanced && IsReservedController(static_cast<uint8_t>(number))))) return false;
        if ((kind == ControllerKind::Rpn || kind == ControllerKind::Nrpn
             || kind == ControllerKind::RpnDelta || kind == ControllerKind::NrpnDelta)
            && number >= 16383) return false;
        if (kind == ControllerKind::Program && number > 16383) return false;
        if (kind == ControllerKind::PolyPressure && number > 127) return false;
        if ((kind == ControllerKind::Bend || kind == ControllerKind::ChannelPressure) && number) return false;
        if (operation == RouteOp::RelativeBase && kind != ControllerKind::Cc7
            && kind != ControllerKind::RpnDelta && kind != ControllerKind::NrpnDelta) return false;
        if ((kind == ControllerKind::RpnDelta || kind == ControllerKind::NrpnDelta)
            && operation != RouteOp::RelativeBase) return false;
        if (kind == ControllerKind::Program && operation != RouteOp::Select
            && !(operation == RouteOp::Trigger && trigger == TriggerEdge::EveryEvent)) return false;
        if (operation == RouteOp::Multiply && (minimum < 0.f || maximum < 0.f || bipolar
            || reduction == Reduction::Sum)) return false;
        if (reduction == Reduction::Product && operation != RouteOp::Multiply) return false;
        if (bipolar && (static_cast<double>(minimum) * maximum > 0.0 || operation == RouteOp::Select
            || operation == RouteOp::Trigger || operation == RouteOp::RelativeBase)) return false;
        if (operation == RouteOp::Select && (bipolar || std::floor(minimum) != minimum
            || std::floor(maximum) != maximum)) return false;
        if (operation == RouteOp::Trigger && (minimum != 0.f || maximum != 1.f || bipolar)) return false;
        if (center != 0xffff && !bipolar) return false;
        if (center != 0xffff) {
            const uint16_t limit = kind == ControllerKind::Cc14 || kind == ControllerKind::Bend
                || kind == ControllerKind::Rpn || kind == ControllerKind::Nrpn ? 16383 : 127;
            if (!center || center >= limit) return false;
        }
        return true;
    }

    bool Matches(const ControllerMessage& m) const {
        if (m.channel > 15 || !(listen.channel_mask & (uint16_t{1} << m.channel))
            || (listen.port != 0xffff && listen.port != m.source.port)
            || (session && session != m.source.session) || number != m.number) return false;
        return kind == m.kind
            || (kind == ControllerKind::Rpn && m.kind == ControllerKind::RpnDelta)
            || (kind == ControllerKind::Nrpn && m.kind == ControllerKind::NrpnDelta);
    }
};

struct ParameterCommand {
    ParamId target = 0;
    RouteId route = 0;
    SourceId source{};
    uint32_t sequence = 0;
    uint8_t channel = 0;
    RouteOp operation = RouteOp::EditBase;
    Reduction reduction = Reduction::LastEvent;
    TriggerEdge trigger = TriggerEdge::Rising;
    float value = 0.f;
};

constexpr int DecodeRelative(uint8_t value, RelativeEncoding encoding) {
    switch (encoding) {
        case RelativeEncoding::TwosComplement: return value < 64 ? value : static_cast<int>(value) - 128;
        case RelativeEncoding::BinaryOffset: return static_cast<int>(value) - 64;
        case RelativeEncoding::SignedBit: return value & 64 ? -static_cast<int>(value & 63) : value;
        case RelativeEncoding::SignedBit2: return value & 64 ? value & 63 : -static_cast<int>(value);
    }
    return 0;
}

template <size_t Capacity = 16>
class Routes {
    static_assert(Capacity > 0, "Routes needs storage");
  public:
    size_t Size() const { return size_; }
    static constexpr size_t MaxSize() { return Capacity; }
    const Route* At(size_t index) const { return index < size_ ? &routes_[index] : nullptr; }
    void Clear() { size_ = 0; }

    bool Add(const Route& route) {
        if (!route.Valid() || size_ == Capacity) return false;
        for (size_t i = 0; i < size_; ++i)
            if (routes_[i].id == route.id || Duplicate(routes_[i], route)) return false;
        routes_[size_++] = route;
        return true;
    }

    bool Remove(RouteId id) {
        for (size_t i = 0; i < size_; ++i) {
            if (routes_[i].id != id) continue;
            for (size_t j = i + 1; j < size_; ++j) routes_[j - 1] = routes_[j];
            --size_;
            return true;
        }
        return false;
    }

    template <typename Registry>
    bool ValidateTargets(const Registry& registry) const {
        for (size_t i = 0; i < size_; ++i) {
            const auto* descriptor = registry.Descriptor(routes_[i].target);
            if (!descriptor || !descriptor->CanRoute(routes_[i])) return false;
        }
        return true;
    }

    template <typename Sink>
    void Process(const ControllerMessage& message, Sink&& sink) const {
        if (!message.Valid()) return;
        for (size_t i = 0; i < size_; ++i) {
            const auto& route = routes_[i];
            if (!route.Matches(message)) continue;
            ParameterCommand command;
            command.target = route.target;
            command.route = route.id;
            command.source = message.source;
            command.sequence = message.sequence;
            command.channel = message.channel;
            command.operation = route.operation;
            command.reduction = route.reduction;
            command.trigger = route.trigger;
            if (message.kind == ControllerKind::RpnDelta || message.kind == ControllerKind::NrpnDelta) {
                if (route.operation != RouteOp::EditBase && route.operation != RouteOp::RelativeBase) continue;
                command.operation = RouteOp::RelativeBase;
                command.value = message.value * route.step;
            } else if (route.operation == RouteOp::RelativeBase) {
                command.value = DecodeRelative(static_cast<uint8_t>(message.value), route.encoding) * route.step;
            } else if (message.kind == ControllerKind::Program && route.operation == RouteOp::Select) {
                const float lower = route.minimum < route.maximum ? route.minimum : route.maximum;
                const float upper = route.minimum > route.maximum ? route.minimum : route.maximum;
                if (message.value < lower || message.value > upper) continue;
                command.value = static_cast<float>(message.value);
            } else {
                const int32_t limit = message.resolution == 14 ? 16383 : 127;
                float norm;
                if (route.bipolar) {
                    const int32_t center = route.center == 0xffff ? (limit + 1) / 2 : route.center;
                    norm = message.value < center
                        ? static_cast<float>(message.value - center) / center
                        : static_cast<float>(message.value - center) / (limit - center);
                } else norm = static_cast<float>(message.value) / static_cast<float>(limit);
                const float magnitude = std::fabs(norm);
                if (route.curve == Curve::Smoothstep)
                    norm = std::copysign(magnitude * magnitude * (3.f - 2.f * magnitude), norm);
                else if (route.curve == Curve::Power)
                    norm = std::copysign(std::pow(magnitude, route.exponent), norm);
                if (route.bipolar) command.value = norm < 0.f ? -norm * route.minimum : norm * route.maximum;
                else command.value = static_cast<float>((1.0 - norm) * route.minimum
                    + static_cast<double>(norm) * route.maximum);
            }
            if (std::isfinite(command.value)) sink(command);
        }
    }

  private:
    static bool Duplicate(const Route& a, const Route& b) {
        return a.target == b.target && a.kind == b.kind && a.number == b.number
            && a.listen.channel_mask == b.listen.channel_mask && a.listen.port == b.listen.port
            && a.session == b.session && a.operation == b.operation;
    }
    std::array<Route, Capacity> routes_{};
    size_t size_ = 0;
};

} // namespace alchemy::midi
