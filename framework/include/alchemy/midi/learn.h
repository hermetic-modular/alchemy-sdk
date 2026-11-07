#pragma once

#include <cstdint>
#include "alchemy/midi/routes.h"

namespace alchemy::midi {

struct LearnOptions {
    Listen listen{};
    ControllerKind kind = ControllerKind::Cc7;
    bool source_specific = true;
    bool channel_specific = true;
    bool advanced = false;
    uint32_t timeout_us = 10000000;
};

enum class LearnStatus : uint8_t { Idle, Armed, Captured, TimedOut };

class LearnSession {
  public:
    bool Arm(const Route& prototype, const LearnOptions& options, uint32_t now_us) {
        if (!prototype.id || !prototype.target || !options.listen.channel_mask
            || options.kind > ControllerKind::PolyPressure || options.timeout_us > 0x7fffffffu) return false;
        Route candidate = prototype;
        candidate.kind = options.kind;
        candidate.number = options.kind == ControllerKind::Cc7 ? 20 : options.kind == ControllerKind::Cc14 ? 16 : 0;
        candidate.listen = options.listen;
        candidate.advanced = options.advanced;
        if (!candidate.Valid()) return false;
        prototype_ = prototype;
        options_ = options;
        deadline_ = now_us + options.timeout_us;
        status_ = LearnStatus::Armed;
        return true;
    }

    void Cancel() { status_ = LearnStatus::Idle; }
    LearnStatus Status() const { return status_; }

    void Poll(uint32_t now_us) {
        if (status_ == LearnStatus::Armed && options_.timeout_us && TimeReached(now_us, deadline_))
            status_ = LearnStatus::TimedOut;
    }

    bool Process(const ControllerMessage& message, uint32_t now_us) {
        Poll(now_us);
        if (!message.Valid() || status_ != LearnStatus::Armed || message.kind != options_.kind || message.channel > 15
            || !(options_.listen.channel_mask & (uint16_t{1} << message.channel))
            || (options_.listen.port != kAnyPort && options_.listen.port != message.source.port)) return false;
        if ((message.kind == ControllerKind::Cc7 || message.kind == ControllerKind::Cc14)
            && !options_.advanced && IsReservedController(static_cast<uint8_t>(message.number))) return false;
        Route candidate = prototype_;
        candidate.kind = message.kind;
        candidate.number = message.number;
        candidate.listen = options_.listen;
        if (options_.source_specific) candidate.listen.port = message.source.port;
        if (options_.channel_specific) candidate.listen.channel_mask = uint16_t{1} << message.channel;
        candidate.session = 0;
        candidate.advanced = options_.advanced;
        if (!candidate.Valid()) return false;
        result_ = candidate;
        status_ = LearnStatus::Captured;
        return true;
    }

    const Route* Result() const { return status_ == LearnStatus::Captured ? &result_ : nullptr; }
    bool Accept() {
        if (status_ != LearnStatus::Captured) return false;
        status_ = LearnStatus::Idle;
        return true;
    }

  private:
    Route prototype_{};
    Route result_{};
    LearnOptions options_{};
    uint32_t deadline_ = 0;
    LearnStatus status_ = LearnStatus::Idle;
};

} // namespace alchemy::midi
