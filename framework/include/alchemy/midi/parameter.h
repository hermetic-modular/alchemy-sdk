#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include "alchemy/midi/routes.h"

namespace alchemy::midi {

enum class ParameterType : uint8_t { Continuous, Selector, Trigger };
constexpr uint32_t OperationBit(RouteOp operation) {
    return operation <= RouteOp::Trigger ? uint32_t{1} << static_cast<uint8_t>(operation) : 0;
}

struct ParameterDescriptor {
    ParamId id = 0;
    ParameterType type = ParameterType::Continuous;
    float minimum = 0.f;
    float maximum = 1.f;
    float initial = 0.f;
    uint32_t operations = OperationBit(RouteOp::EditBase) | OperationBit(RouteOp::RelativeBase)
        | OperationBit(RouteOp::AddOffset) | OperationBit(RouteOp::Overlay);
    bool persistent = true;

    bool Valid() const {
        if (!id || type > ParameterType::Trigger || !std::isfinite(minimum)
            || !std::isfinite(maximum) || !std::isfinite(initial)
            || minimum >= maximum || initial < minimum || initial > maximum
            || !operations || (operations & ~uint32_t{0x7f})) return false;
        if (type == ParameterType::Selector && (minimum < 0.f || maximum > 16777215.f
            || std::floor(minimum) != minimum
            || std::floor(maximum) != maximum || std::floor(initial) != initial
            || (operations & OperationBit(RouteOp::Multiply)))) return false;
        if (type == ParameterType::Continuous && (operations & OperationBit(RouteOp::Select))) return false;
        if (type == ParameterType::Trigger) return operations == OperationBit(RouteOp::Trigger);
        return !(operations & OperationBit(RouteOp::Trigger));
    }

    bool CanRoute(const Route& route) const {
        if (!Valid() || !route.Valid() || route.target != id || !(operations & OperationBit(route.operation))) return false;
        if ((route.kind == ControllerKind::Rpn || route.kind == ControllerKind::Nrpn)
            && route.operation == RouteOp::EditBase && !(operations & OperationBit(RouteOp::RelativeBase))) return false;
        if (route.operation == RouteOp::EditBase || route.operation == RouteOp::Overlay || route.operation == RouteOp::Select)
            return route.minimum >= minimum && route.minimum <= maximum && route.maximum >= minimum && route.maximum <= maximum;
        return true;
    }
};

struct ParameterBase { ParamId id = 0; float value = 0.f; };

struct ParameterResult {
    bool ok = false;
    bool triggered = false;
    bool trigger_level = false;
    float value = 0.f;
    explicit operator bool() const { return ok; }
};

template <size_t Targets = 32, size_t Lanes = 64>
class ParameterMixer {
    static_assert(Targets > 0 && Lanes > 0 && Lanes <= 65535, "Invalid parameter capacity");
    struct Target {
        ParameterDescriptor descriptor{};
        float base = 0.f;
    };
    struct Lane {
        bool used = false;
        SourceId source{};
        uint8_t channel = 0;
        uint16_t group = 0;
        uint64_t order = 0;
        float value = 0.f;
    };
    struct Group {
        ParamId target = 0;
        RouteId route = 0;
        RouteOp operation = RouteOp::AddOffset;
        Reduction reduction = Reduction::LastEvent;
        uint64_t order = 0;
        double value = 0.0;
    };

  public:
    bool Register(const ParameterDescriptor& descriptor) {
        if (!descriptor.Valid() || Find(descriptor.id)) return false;
        for (auto& target : targets_) {
            if (target.descriptor.id) continue;
            target.descriptor = descriptor;
            target.base = descriptor.initial;
            return true;
        }
        return false;
    }

    const ParameterDescriptor* Descriptor(ParamId id) const {
        const auto* target = Find(id);
        return target ? &target->descriptor : nullptr;
    }

    bool SetBase(ParamId id, float value) {
        auto* target = Find(id);
        if (!target || !std::isfinite(value) || target->descriptor.type == ParameterType::Trigger) return false;
        target->base = Clamp(value, target->descriptor);
        return true;
    }

    float Base(ParamId id) const {
        const auto* target = Find(id);
        return target ? target->base : 0.f;
    }

    template <typename Sink>
    void VisitPersistentBases(Sink&& sink) const {
        for (const auto& target : targets_)
            if (target.descriptor.id && target.descriptor.persistent && target.descriptor.type != ParameterType::Trigger)
                sink(ParameterBase{target.descriptor.id, target.base});
    }

    bool RestoreBases(const ParameterBase* bases, size_t count) {
        if ((count && !bases) || count > Targets) return false;
        for (size_t i = 0; i < count; ++i) {
            const auto* target = Find(bases[i].id);
            if (!target || !target->descriptor.persistent || target->descriptor.type == ParameterType::Trigger
                || !std::isfinite(bases[i].value) || bases[i].value < target->descriptor.minimum
                || bases[i].value > target->descriptor.maximum
                || (target->descriptor.type == ParameterType::Selector && std::floor(bases[i].value) != bases[i].value)) return false;
            for (size_t j = 0; j < i; ++j) if (bases[j].id == bases[i].id) return false;
        }
        for (size_t i = 0; i < count; ++i) Find(bases[i].id)->base = bases[i].value;
        return true;
    }

    ParameterResult Apply(const ParameterCommand& command) {
        auto* target = Find(command.target);
        if (!target || !command.route || command.source.port == kAnyPort || !command.source.session
            || command.channel > 15 || !std::isfinite(command.value)
            || command.operation > RouteOp::Trigger || command.reduction > Reduction::Product
            || command.trigger > TriggerEdge::EveryEvent
            || !(target->descriptor.operations & OperationBit(command.operation))
            || (command.reduction == Reduction::Product && command.operation != RouteOp::Multiply)
            || (command.operation == RouteOp::Multiply && (command.value < 0.f || command.reduction == Reduction::Sum))) return {};
        if (command.operation == RouteOp::EditBase || command.operation == RouteOp::Select)
            return {SetBase(command.target, command.value), false, false, Base(command.target)};
        if (command.operation == RouteOp::RelativeBase) {
            const double value = static_cast<double>(target->base) + command.value;
            target->base = static_cast<float>(value < target->descriptor.minimum ? target->descriptor.minimum
                : value > target->descriptor.maximum ? target->descriptor.maximum : value);
            target->base = Clamp(target->base, target->descriptor);
            return {true, false, false, target->base};
        }
        const size_t index = FindGroup(command);
        if (index == Lanes) return {};
        if (groups_[index].route && (groups_[index].operation != command.operation
            || groups_[index].reduction != command.reduction)) return {};
        Lane* lane = FindLane(command, index);
        if (!lane) return {};
        const bool previous = lane->used && lane->value >= 0.5f;
        auto& group = groups_[index];
        group.target = command.target; group.route = command.route;
        group.operation = command.operation; group.reduction = command.reduction;
        lane->used = true; lane->source = command.source; lane->channel = command.channel;
        lane->group = static_cast<uint16_t>(index); lane->order = ++order_; lane->value = command.value;
        Rebuild(index);
        if (command.operation != RouteOp::Trigger) return {true, false, false, command.value};
        const bool level = command.value >= 0.5f;
        bool trigger = false;
        switch (command.trigger) {
            case TriggerEdge::Rising: trigger = !previous && level; break;
            case TriggerEdge::Falling: trigger = previous && !level; break;
            case TriggerEdge::Momentary: trigger = previous != level; break;
            case TriggerEdge::EveryEvent: trigger = true; break;
        }
        return {true, trigger, level, command.value};
    }

    float Value(ParamId id, float lock = 0.f, float cv = 0.f) const {
        const auto* target = Find(id);
        if (!target) return 0.f;
        const double result = Compose(id,lock,cv);
        const float limited = static_cast<float>(result < target->descriptor.minimum ? target->descriptor.minimum
            : result > target->descriptor.maximum ? target->descriptor.maximum : result);
        return Clamp(limited, target->descriptor);
    }

    // Leaves final limiting to an audio-rate CV or modulation composition stage.
    double Compose(ParamId id, float lock = 0.f, float cv = 0.f) const {
        const auto* target = Find(id);
        if (!target) return 0.f;
        if (!std::isfinite(lock) || !std::isfinite(cv)) return target->base;
        double base = target->base;
        uint64_t newest_overlay = 0;
        double offsets = 0.0, factor = 1.0;
        bool zero_factor = false;
        for (const auto& group : groups_) {
            if (!group.route || group.target != id) continue;
            if (group.operation == RouteOp::Overlay && group.order > newest_overlay) {
                newest_overlay = group.order; base = group.value;
            } else if (group.operation == RouteOp::AddOffset) offsets += group.value;
            else if (group.operation == RouteOp::Multiply) {
                zero_factor |= group.value == 0.0;
                factor *= group.value;
            }
        }
        const double additive = base + lock + cv + offsets;
        const double result = zero_factor || additive == 0.0 ? 0.0 : additive * factor;
        if (std::isnan(result)) return target->base;
        return result;
    }

    void ResetSource(SourceId source, uint8_t channel = kAnyChannel) {
        std::array<bool, Lanes> changed{};
        for (auto& lane : lanes_)
            if (lane.used && lane.source == source && (channel == kAnyChannel || lane.channel == channel)) {
                changed[lane.group] = true; lane = {};
            }
        for (size_t i = 0; i < Lanes; ++i) if (changed[i]) Rebuild(i);
    }

    void RemoveRoute(RouteId route) {
        for (size_t i = 0; i < Lanes; ++i) {
            if (!groups_[i].route || groups_[i].route != route) continue;
            for (auto& lane : lanes_) if (lane.used && lane.group == i) lane = {};
            groups_[i] = {};
        }
    }

    void ClearModulation() { lanes_ = {}; groups_ = {}; }

  private:
    static float Clamp(float value, const ParameterDescriptor& descriptor) {
        if (value < descriptor.minimum) value = descriptor.minimum;
        if (value > descriptor.maximum) value = descriptor.maximum;
        return descriptor.type == ParameterType::Selector ? std::round(value) : value;
    }
    Target* Find(ParamId id) {
        if (!id) return nullptr;
        for (auto& target : targets_) if (target.descriptor.id == id) return &target;
        return nullptr;
    }
    const Target* Find(ParamId id) const {
        if (!id) return nullptr;
        for (const auto& target : targets_) if (target.descriptor.id == id) return &target;
        return nullptr;
    }
    size_t FindGroup(const ParameterCommand& command) const {
        size_t free = Lanes;
        for (size_t i = 0; i < Lanes; ++i) {
            const auto& group = groups_[i];
            if (group.target == command.target && group.route == command.route) return i;
            if (!group.route && free == Lanes) free = i;
        }
        return free;
    }
    Lane* FindLane(const ParameterCommand& command, size_t group) {
        Lane* free = nullptr;
        for (auto& lane : lanes_) {
            if (lane.used && lane.group == group && lane.source == command.source && lane.channel == command.channel) return &lane;
            if (!lane.used && !free) free = &lane;
        }
        return free;
    }
    void Rebuild(size_t index) {
        auto& group = groups_[index];
        bool any = false, zero = false;
        double value = group.reduction == Reduction::Product ? 1.0 : 0.0;
        uint64_t newest = 0;
        for (const auto& lane : lanes_) {
            if (!lane.used || lane.group != index) continue;
            switch (group.reduction) {
                case Reduction::LastEvent:
                    if (!any || lane.order > newest) value = lane.value;
                    break;
                case Reduction::Maximum:
                    if (!any || lane.value > value) value = lane.value;
                    break;
                case Reduction::Sum: value += lane.value; break;
                case Reduction::Product: zero |= lane.value == 0.f; value *= lane.value; break;
            }
            newest = lane.order > newest ? lane.order : newest;
            any = true;
        }
        if (!any) { group = {}; return; }
        group.value = zero ? 0.0 : value; group.order = newest;
    }
    std::array<Target, Targets> targets_{};
    std::array<Lane, Lanes> lanes_{};
    std::array<Group, Lanes> groups_{};
    uint64_t order_ = 0;
};

} // namespace alchemy::midi
