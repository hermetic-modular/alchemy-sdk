/**
 * @file trigger_jack.h
 * @brief alchemy::TriggerJack — codec-input edge detector for J1, J2.
 *
 * AC coupling removes held DC levels. Detect fast positive changes rather
 * than absolute voltage, with hysteresis and ringing rejection. No threshold
 * setup is required. Capture runs in AlchemyLabV2::StartAudio()'s shim;
 * poll RisingEdge() from OnPoll or the audio callback.
 */

#pragma once

#include <atomic>
#include <cstddef>

namespace alchemy {

class AlchemyLabV2;

class TriggerJack
{
  public:
    /** True if a rise occurred since the last read; atomically clears the
     *  latch. Multiple rises between reads coalesce. Read once and reuse
     *  the result for application logic and logging. */
    bool RisingEdge();

  private:
    friend class AlchemyLabV2;

    void Init(float sample_rate);
    void ProcessBlock(const float* samples, size_t n);
    bool ProcessSample(float sample);

    static constexpr size_t kMaxWindow = 10u;
    static_assert(std::atomic<bool>::is_always_lock_free);

    float history_[kMaxWindow] = {};
    size_t window_ = 2u;
    size_t position_ = 0u;
    float decay_ = 0.0f;
    float envelope_ = 0.0f;
    bool armed_ = true;
    bool primed_ = false;
    std::atomic<bool> latched_{false};
};

} // namespace alchemy
