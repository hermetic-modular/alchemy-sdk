/**
 * @file alchemy/led/anims/clip_indicator.cpp
 */

#include "alchemy/led/anims/clip_indicator.h"

#include <cstring>

namespace alchemy {

static_assert(sizeof(float) == sizeof(uint32_t),
              "the Process fast path puns float bits into uint32_t");

void ClipIndicator::SetEnabled(bool on)
{
    enabled_ = on;
    Reset();
}

void ClipIndicator::Reset()
{
    seen_count_        = clip_count_;
    credit_            = 0.0f;
    hold_remaining_ms_ = 0.0f;
    lit_               = false;
}

void ClipIndicator::Process(const float* in, size_t frames)
{
    if (!enabled_ || in == nullptr) return;

    float t = threshold_ < 0.0f ? 0.0f : threshold_;
    uint32_t t_bits;
    std::memcpy(&t_bits, &t, sizeof t_bits);

    /* Stride-spaced probe: each hit stands for `stride` samples, so the
     * count Tick consumes stays in true-sample units. */
    const size_t stride = stride_;
    uint32_t     hits   = 0u;
    for (size_t i = 0; i < frames; i += stride)
    {
        uint32_t u;
        std::memcpy(&u, &in[i], sizeof u);
        if ((u & 0x7FFFFFFFu) >= t_bits) hits++;
    }
    /* Single word store; Tick only ever reads this member. */
    if (hits != 0u)
        clip_count_ = clip_count_ + hits * static_cast<uint32_t>(stride);
}

void ClipIndicator::Process(const float* const* in,
                            size_t              num_channels,
                            size_t              frames)
{
    if (in == nullptr) return;
    for (size_t ch = 0; ch < num_channels; ch++)
        Process(in[ch], frames);
}

void ClipIndicator::Tick(float dt_ms)
{
    if (!enabled_) return;

    const uint32_t now   = clip_count_;
    const uint32_t fresh = now - seen_count_;
    seen_count_          = now;

    /* Leaky bucket: drains fully over the window, capped at the trigger
     * level so release after heavy clipping stays bounded. */
    const float trigger = static_cast<float>(min_clipped_samples_);
    credit_ -= (trigger / window_ms_) * dt_ms;
    if (credit_ < 0.0f) credit_ = 0.0f;
    credit_ += static_cast<float>(fresh);
    if (credit_ > trigger) credit_ = trigger;

    const bool triggered = credit_ >= trigger;
    if (triggered)
        hold_remaining_ms_ = hold_ms_;
    else
    {
        hold_remaining_ms_ -= dt_ms;
        if (hold_remaining_ms_ < 0.0f) hold_remaining_ms_ = 0.0f;
    }

    lit_ = triggered || hold_remaining_ms_ > 0.0f;
}

void ClipIndicator::Draw(LedPanel& panel, LedPanel::Rgb color) const
{
    if (!enabled_ || !lit_) return;

    const LedPanel::Rgb c = panel.ScaleGlobal(color);

    uint32_t pots = pot_mask_;
    for (uint8_t p = 0u; pots != 0u; p++, pots >>= 1u)
        if (pots & 1u)
            panel.SetRingByHour(p, pip_hour_, c);

    uint32_t btns = button_mask_;
    for (uint8_t b = 0u; btns != 0u; b++, btns >>= 1u)
    {
        if (!(btns & 1u)) continue;
        switch (accent_)
        {
            case ButtonAccent::Bottom:
                panel.SetButton(b, LedPanel::ButtonLed::Bottom, c);
                break;
            case ButtonAccent::Top:
                panel.SetButton(b, LedPanel::ButtonLed::Top, c);
                break;
            case ButtonAccent::Pair:
                panel.SetButtonPair(b, c);
                break;
        }
    }
}

} // namespace alchemy
