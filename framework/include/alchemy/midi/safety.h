#pragma once

#include <array>
#include <cstddef>
#include "alchemy/midi/types.h"

namespace alchemy::midi {

template <size_t Sources = 4>
class ActiveSensing {
    static_assert(Sources > 0, "Active sensing needs bounded source storage");
    struct State {
        SourceId source{};
        uint32_t last_us = 0;
        bool armed = false;
    };
  public:
    static constexpr uint32_t kTimeoutUs = 300000;

    void Touch(SourceId source, uint32_t time_us) {
        for (auto& state : states_)
            if (state.armed && state.source == source) state.last_us = time_us;
    }

    bool Observe(const Event& event) {
        if (!event.Valid()) return false;
        for (auto& state : states_) {
            if (!state.armed || state.source != event.source) continue;
            if (event.type == EventType::SourceReset || event.type == EventType::SystemReset)
                state.armed = false;
            else state.last_us = event.time_us;
            return true;
        }
        if (event.type != EventType::ActiveSensing) return true;
        for (auto& state : states_) {
            if (state.armed) continue;
            state = {event.source, event.time_us, true};
            return true;
        }
        return false;
    }

    template <typename Sink>
    size_t Poll(uint32_t now_us, Sink&& sink) {
        size_t count = 0;
        for (auto& state : states_) {
            if (!state.armed || !TimeReached(now_us, state.last_us + kTimeoutUs)) continue;
            state.armed = false;
            sink(Event::System(EventType::SourceReset, state.source, now_us));
            ++count;
        }
        return count;
    }

    void Clear() { states_ = {}; }
    void ResetSource(SourceId source) {
        for (auto& state : states_) if (state.source == source) state = {};
    }

  private:
    std::array<State, Sources> states_{};
};

} // namespace alchemy::midi
