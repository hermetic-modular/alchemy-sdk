#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include "alchemy/midi/routes.h"

namespace alchemy::midi {

struct TakeoverResult {
    bool ok = false;
    bool accepted = false;
    ParameterCommand command{};
};

template <size_t Capacity = 16>
class TakeoverTracker {
    static_assert(Capacity > 0, "Takeover storage must be nonempty");
    struct State {
        bool used = false, caught = false, has_input = false;
        ParamId target = 0;
        RouteId route = 0;
        SourceId source{};
        uint8_t channel = 0;
        uint32_t revision = 0;
        float previous = 0.f, anchor_input = 0.f, anchor_target = 0.f;
    };
  public:
    TakeoverResult Resolve(const ParameterCommand& command, const Route& route,
                           float base, uint32_t revision) {
        if (!route.Valid() || command.target != route.target || command.route != route.id
            || command.source.port == kAnyPort || !command.source.session || command.channel > 15
            || !(route.listen.channel_mask & (uint16_t{1} << command.channel))
            || (route.listen.port != kAnyPort && route.listen.port != command.source.port)
            || (route.session && route.session != command.source.session)
            || command.reduction != route.reduction || command.trigger != route.trigger
            || (command.operation != route.operation && !(command.operation == RouteOp::RelativeBase
                && route.operation == RouteOp::EditBase &&
                (route.kind == ControllerKind::Rpn || route.kind == ControllerKind::Nrpn)))
            || !std::isfinite(command.value) || !std::isfinite(base)) return {};
        const float lower = route.minimum < route.maximum ? route.minimum : route.maximum;
        const float upper = route.minimum > route.maximum ? route.minimum : route.maximum;
        if (command.operation != RouteOp::RelativeBase &&
            (command.value < lower || command.value > upper)) return {};
        if (route.takeover == TakeoverPolicy::Immediate || command.operation == RouteOp::RelativeBase)
            return {true, true, command};
        if (command.operation != RouteOp::EditBase) return {};
        if (base < lower || base > upper) return {true, false, command};
        auto* state = Find(command);
        if (!state) return {};
        const double span = static_cast<double>(route.maximum) - route.minimum;
        const float input = Clamp(static_cast<float>((command.value - static_cast<double>(route.minimum)) / span));
        const float target = Clamp(static_cast<float>((base - static_cast<double>(route.minimum)) / span));
        if (!state->used || state->revision != revision) {
            state->caught = false;
            state->anchor_input = state->has_input ? state->previous : input;
            state->anchor_target = target;
            state->revision = revision;
        }
        state->used = true; state->target = command.target; state->route = command.route;
        state->source = command.source; state->channel = command.channel;
        TakeoverResult result{true, false, command};
        if (route.takeover == TakeoverPolicy::Pickup) {
            if (!state->caught && (std::fabs(input - target) <= route.pickup_tolerance
                || (state->has_input && (state->previous - target) * (input - target) <= 0.f))) state->caught = true;
            result.accepted = state->caught;
        } else {
            const float mapped = input >= state->anchor_input
                ? state->anchor_target + (input - state->anchor_input) * (1.f - state->anchor_target)
                    / (state->anchor_input < 1.f ? 1.f - state->anchor_input : 1.f)
                : state->anchor_target * input / (state->anchor_input > 0.f ? state->anchor_input : 1.f);
            const double fraction = Clamp(mapped);
            result.command.value = static_cast<float>((1.0 - fraction) * route.minimum + fraction * route.maximum);
            result.accepted = true;
        }
        state->previous = input; state->has_input = true;
        return result;
    }

    bool Acknowledge(const ParameterCommand& command, uint32_t revision) {
        for (auto& state : states_)
            if (Matches(state, command)) { state.revision = revision; return true; }
        return false;
    }
    void ResetSource(SourceId source, uint8_t channel = kAnyChannel) {
        for (auto& state : states_)
            if (state.used && state.source == source && (channel == kAnyChannel || state.channel == channel)) state = {};
    }
    void RemoveRoute(RouteId route) {
        for (auto& state : states_) if (state.used && state.route == route) state = {};
    }
    void Clear() { states_ = {}; }

  private:
    static float Clamp(float value) { return value < 0.f ? 0.f : value > 1.f ? 1.f : value; }
    static bool Matches(const State& state, const ParameterCommand& command) {
        return state.used && state.target == command.target && state.route == command.route
            && state.source == command.source && state.channel == command.channel;
    }
    State* Find(const ParameterCommand& command) {
        State* free = nullptr;
        for (auto& state : states_) {
            if (Matches(state, command)) return &state;
            if (!state.used && !free) free = &state;
        }
        return free;
    }
    std::array<State, Capacity> states_{};
};

} // namespace alchemy::midi
