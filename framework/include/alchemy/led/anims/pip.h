/**
 * @file pip.h
 * @brief DrawPip: single LED (or narrow band) position indicator.
 *
 * Renders a small lit region at a position along the arc proportional
 * to a normalized value.  Supports optional blinking.
 *
 * Three optional polish fields turn the basic LED-stepping pip into a
 * smoothly gliding LFO / motion indicator:
 *
 *   smooth          — when true and width == 1, the pip is split across
 *                     the two LEDs nearest the fractional position with
 *                     weights that sum to 1.  Eliminates LED-by-LED
 *                     stepping as the value changes.
 *   tail_intensity  — when > 0 (and smooth == true), anti-aliased "comet"
 *                     tails one step further out on each side.  ~0.2 is
 *                     enough to soften fast motion without smearing.
 *   background      — when not {0,0,0}, every step on the arc is first
 *                     filled with this color and the pip is blended on
 *                     top.  Leaves underlying pixels untouched at {0,0,0}
 *                     so the pip can layer over another animation.
 *
 * Usage — basic:
 *   alchemy::PipDesc desc;
 *   desc.color    = {0xFF, 0xFF, 0x00};
 *   desc.width    = 1u;
 *   desc.blink_hz = 2.0f;   // 2 Hz blink
 *   alchemy::DrawPip(leds, pot_idx, kAlchemyLabArcGeometry, value, desc, t_ms);
 *
 * Usage — smoothly-gliding LFO pip with comet tails over a dim ring:
 *   alchemy::PipDesc desc;
 *   desc.color          = {0x40, 0xFF, 0x80};
 *   desc.smooth         = true;
 *   desc.tail_intensity = 0.20f;
 *   desc.background     = {0x02, 0x05, 0x02};
 *   // Bipolar LFO output in [-1, +1] → normalised to [0, 1].
 *   alchemy::DrawPip(leds, pot_idx, kAlchemyLabArcGeometry,
 *                    lfo * 0.5f + 0.5f, desc);
 */

#pragma once

#include <cstdint>
#include "alchemy/hardware_types.h"
#include "alchemy/led/panel.h"

namespace alchemy {

/* ── Compose mode ───────────────────────────────────────────────────────── */

enum class PipCompose : uint8_t
{
    Replace,  ///< The pip owns its LEDs (blends over `background` if set).
    Add,      ///< Saturating add — rides on top of what this frame drew.
    Carve,    ///< Subtractive — dims what this frame drew (a notch).
              ///< Both compose against the current RingFrame contents
              ///< only; the frame cannot read the panel back.
};

/* ── Motion mapping ─────────────────────────────────────────────────────── */

enum class PipMotion : uint8_t
{
    Direct,  ///< Position used as-is (clamped 0..1).
    Wrap,    ///< Fractional part — a phase laps the region (cursor).
    Bounce,  ///< Triangle-folded — a phase ping-pongs end to end (comet).
};

/* ── Trail side ─────────────────────────────────────────────────────────── */

enum class PipTrailSide : uint8_t
{
    Both,  ///< Symmetric comet tails.
    Ccw,   ///< Trail only toward the CCW end.
    Cw,    ///< Trail only toward the CW end.
};

/* ── Pip descriptor ─────────────────────────────────────────────────────── */

struct PipDesc
{
    LedPanel::Rgb color           = {0xFF, 0xFF, 0xFF};  ///< Pip color.
    uint8_t       width           = 1u;                   ///< Number of LEDs to light.
    float         blink_hz        = 0.0f;                 ///< 0 = always on; >0 = blink at this rate.

    /* Opt-in polish — all defaults preserve the original quantised behavior. */

    bool          smooth          = false;                ///< Sub-LED two-tap interpolation (width == 1 only).
    float         tail_intensity  = 0.0f;                 ///< One-step comet tails, 0..1; sugar for {trail_steps = 1, trail_falloff}.
    LedPanel::Rgb background      = {0u, 0u, 0u};         ///< Dim full-arc fill; {0,0,0} = leave underlying pixels alone.

    /* Composition (RingFrame path; DrawPip uses the defaults). */

    PipCompose    compose         = PipCompose::Replace;  ///< How the pip lands on the frame.
    PipMotion     motion          = PipMotion::Direct;    ///< Mapping applied to the position input.
    uint8_t       trail_steps     = 0u;                   ///< Trail length in LEDs; 0 = none (or tail_intensity sugar).
    float         trail_falloff   = 0.5f;                 ///< Per-LED trail decay 0..1.
    PipTrailSide  trail_side      = PipTrailSide::Both;   ///< Which side(s) the trail extends.
};

/* ── DrawPip ────────────────────────────────────────────────────────────── */

/**
 * Draw a small position-indicator pip on a pot ring.
 *
 * @param dst        Target LED panel.
 * @param pot_idx    Ring index (0-based).
 * @param start_hour Clock-face hour of the CCW arc endpoint.
 * @param step_hours Hours per arc step.
 * @param arc_leds   Total arc steps (LED count for the ring).
 * @param value      Normalized value 0..1; maps to position along the arc.
 * @param desc       Pip style, color, width, and blink rate.
 * @param t_ms       Absolute time in milliseconds (drives blink).
 */
void DrawPip(LedPanel&      dst,
             uint8_t        pot_idx,
             float          start_hour,
             float          step_hours,
             uint8_t        arc_leds,
             float          value,
             const PipDesc& desc,
             uint32_t       t_ms = 0u);

/** ArcGeometry overload — replaces the three geometry scalar arguments. */
inline void DrawPip(LedPanel&          dst,
                    uint8_t            pot_idx,
                    const ArcGeometry& geo,
                    float              value,
                    const PipDesc&     desc,
                    uint32_t           t_ms = 0u)
{
    DrawPip(dst, pot_idx, geo.start_hour, geo.step_hours, geo.arc_leds,
            value, desc, t_ms);
}

} // namespace alchemy
