/**
 * @file trigger_jack.cpp
 * @brief TriggerJack — AC-coupled codec trigger detection.
 */

#include "alchemy/hw/trigger_jack.h"
#include <cmath>

namespace alchemy {

namespace {
/* A 50 us difference rejects coupling droop while preserving gate edges.
 * The 1 ms envelope rejects ringing after either polarity without imposing
 * a fixed retrigger lockout. Samples are nominally 1.0 = 5 V at the jack. */
constexpr float kWindowSeconds = 0.00005f;
constexpr float kRingDecaySeconds = 0.001f;
constexpr float kRiseSample = 0.1f;
constexpr float kReleaseSample = 0.025f;
constexpr float kRingFraction = 0.35f;
}

void TriggerJack::Init(float sample_rate)
{
    if (!(sample_rate >= 8000.f && sample_rate <= 192000.f)) sample_rate = 48000.f;
    window_ = static_cast<size_t>(sample_rate * kWindowSeconds + 0.5f);
    if (window_ < 1u) window_ = 1u;
    if (window_ > kMaxWindow) window_ = kMaxWindow;
    decay_ = std::exp(-1.f / (sample_rate * kRingDecaySeconds));
    position_ = 0u;
    envelope_ = 0.f;
    armed_ = true;
    primed_ = false;
    latched_.store(false, std::memory_order_relaxed);
}

bool TriggerJack::RisingEdge()
{
    return latched_.exchange(false, std::memory_order_relaxed);
}

bool TriggerJack::ProcessSample(float sample)
{
    if (!std::isfinite(sample))
    {
        primed_ = false;
        armed_ = true;
        envelope_ = 0.f;
        return false;
    }
    if (!primed_)
    {
        for (float& value : history_) value = sample;
        primed_ = true;
        return false;
    }

    const float change = sample - history_[position_];
    history_[position_] = sample;
    if (++position_ == window_) position_ = 0u;
    envelope_ = std::fmax(envelope_ * decay_, std::fabs(change));
    const float threshold = std::fmax(kRiseSample, envelope_ * kRingFraction);
    if (change <= kReleaseSample) armed_ = true;
    if (armed_ && change >= threshold)
    {
        armed_ = false;
        return true;
    }
    return false;
}

void TriggerJack::ProcessBlock(const float* samples, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (ProcessSample(samples[i]))
            latched_.store(true, std::memory_order_relaxed);
}

} // namespace alchemy
