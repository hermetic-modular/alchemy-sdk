/**
 * @file alchemy/led/anims/clip_indicator.h
 * @brief alchemy::ClipIndicator - audio-fed clipping light with transient
 *        suppression and a minimum hold.
 *
 *   THRESHOLD    |sample| ≥ the threshold counts as clipped (default
 *                0.98 of full scale.
 *
 *   SUPPRESSION  clipped samples feed a leaky bucket that drains fully
 *                over a window (default 50 ms).  The light fires only
 *                once enough of them accumulate (default 8)
 *
 *   HOLD         once fired, the light stays on at least `Hold` ms
 *                (default 50) and retriggers while clipping persists.
 *
 *   STRIDE       Process probes every few samples rather than all of
 *                them.  The stride is derived from the suppression
 *                config (min_clipped_samples / 2, capped at 16 — so 4
 *                for the default 8) and each hit counts as stride
 *                samples, keeping counts in true-sample units.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "alchemy/led/panel.h"

namespace alchemy {

class ClipIndicator
{
  public:
    /** Which accent LED(s) light on a button passed to Buttons(). */
    enum class ButtonAccent : uint8_t
    {
        Bottom,
        Top,
        Pair,
    };

    ClipIndicator() = default;

    ClipIndicator& Threshold(float frac)
    {
        threshold_ = frac;
        return *this;
    }

    ClipIndicator& Suppression(uint16_t min_clipped_samples,
                               float    window_ms = 50.0f)
    {
        min_clipped_samples_ = min_clipped_samples ? min_clipped_samples : 1u;
        window_ms_           = window_ms < 1.0f ? 1.0f : window_ms;

        const uint16_t s = min_clipped_samples_ / 2u;
        stride_ = static_cast<uint8_t>(s < 1u ? 1u : (s > 16u ? 16u : s));
        return *this;
    }

    ClipIndicator& Hold(float ms)
    {
        hold_ms_ = ms < 0.0f ? 0.0f : ms;
        return *this;
    }

    template <typename... Pot>
    ClipIndicator& Pots(Pot... pots)
    {
        pot_mask_ = (0u | ... | Bit(static_cast<uint32_t>(pots)));
        return *this;
    }

    ClipIndicator& AllPots()
    {
        pot_mask_ = 0xFFFFFFFFu;
        return *this;
    }

    template <typename... Btn>
    ClipIndicator& Buttons(Btn... btns)
    {
        button_mask_ = (0u | ... | Bit(static_cast<uint32_t>(btns)));
        return *this;
    }

    ClipIndicator& Accent(ButtonAccent a)
    {
        accent_ = a;
        return *this;
    }

    ClipIndicator& PipHour(float hour)
    {
        pip_hour_ = hour;
        return *this;
    }

    ClipIndicator& Color(LedPanel::Rgb c)
    {
        color_ = c;
        return *this;
    }

    /* ── Runtime switch ──────────────────────────────────────────────── */
    void SetEnabled(bool on);
    bool Enabled() const { return enabled_; }
    void Reset();

    /* ── Audio side ──────────────────────────────────────────────────── */
    void Process(const float* in, size_t frames);
    void Process(const float* const* in, size_t num_channels, size_t frames);

    /* ── Control side ────────────────────────────────────────────────── */
    void Tick(float dt_ms);
    bool Lit() const { return lit_; }
    void Draw(LedPanel& panel) const { Draw(panel, color_); }
    void Draw(LedPanel& panel, LedPanel::Rgb color) const;

  private:
    /** Guarded single-bit mask (indices ≥ 32 contribute nothing). */
    static constexpr uint32_t Bit(uint32_t idx)
    {
        return idx < 32u ? (1u << idx) : 0u;
    }

    /* ── Configuration ──────────────────────────────────────────────── */
    float         threshold_           = 0.98f;
    uint16_t      min_clipped_samples_ = 8u;
    float         window_ms_           = 50.0f;
    float         hold_ms_             = 50.0f;
    uint8_t       stride_              = 4u;   ///< Derived: min_clipped/2 in [1,16].
    uint32_t      pot_mask_            = 0xFFFFFFFFu;
    uint32_t      button_mask_         = 0u;
    ButtonAccent  accent_              = ButtonAccent::Pair;
    float         pip_hour_            = 6.0f;
    LedPanel::Rgb color_               = {0xFF, 0x00, 0x00};
    bool          enabled_             = true;

    /* ── ISR ↔ poll handoff ─────────────────────────────────────────── */
    volatile uint32_t clip_count_ = 0u;  ///< Total clipped samples; wraps.

    /* ── Tick-side state ────────────────────────────────────────────── */
    uint32_t seen_count_        = 0u;    ///< clip_count_ at the last Tick.
    float    credit_            = 0.0f;  ///< Suppression bucket fill.
    float    hold_remaining_ms_ = 0.0f;
    bool     lit_               = false;
};

} // namespace alchemy
