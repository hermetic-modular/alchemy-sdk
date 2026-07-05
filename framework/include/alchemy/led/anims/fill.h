/**
 * @file fill.h
 * @brief Unified ring fill drawing — Edge and Center fill modes.
 *
 * A ring always displays a fill: some LEDs in an active region, the rest
 * in a passive region.  All fill shapes are configurations of this idea:
 *
 *   FillMode::Edge    — unipolar fill from the CCW stop toward CW.
 *                       value 0..1.
 *   FillMode::Center  — bipolar fan from a pivot step outward in two arms.
 *                       value -1..+1 (0 = pivot only).
 *
 * Per-frame animation on the active region:
 *
 *   FillAnim::None    — static.
 *   FillAnim::Pulse   — uniform amplitude breath, slow rise and fall (~2 s).
 *   FillAnim::Ripple  — per-LED phase-offset wave, organic movement.
 *
 * anim_depth (0..1) scales the modulation amplitude for Pulse and Ripple.
 *
 * FillCompose controls how passive pixels are written:
 *   FillCompose::Replace — writes all pixels (active and passive).
 *   FillCompose::Overlay — writes only active pixels; passive region untouched.
 *                          Use for VU-overlay behavior.
 *
 * Usage:
 *   alchemy::FillDesc desc;
 *   desc.mode     = alchemy::FillMode::Edge;
 *   desc.color    = {0x00, 0xFF, 0xC0};
 *   desc.anim     = alchemy::FillAnim::Pulse;
 *   desc.anim_depth = 0.5f;
 *   alchemy::DrawFill(leds, pot_idx, kAlchemyLabArcGeometry, value, desc, t_ms);
 */

#pragma once

#include <cstdint>
#include "alchemy/hardware_types.h"
#include "alchemy/led/panel.h"

namespace alchemy {

/* ── Fill modes ─────────────────────────────────────────────────────────── */

enum class FillMode : uint8_t
{
    Edge,    ///< Unipolar fill from CCW end toward CW.  value 0..1.
    Center,  ///< Bipolar fan outward from pivot step.   value -1..+1.
};

/* ── Animation on the active region ────────────────────────────────────── */

enum class FillAnim : uint8_t
{
    None,    ///< Static — no per-frame modulation.
    Pulse,   ///< Uniform amplitude breath, steady rise and fall over ~2 s.
    Ripple,  ///< Per-LED phase-offset sine wave — organic movement.
};

/* ── Fill direction (Edge mode) ─────────────────────────────────────────── */

enum class FillDirection : uint8_t
{
    Cw,   ///< Fill grows from the CCW stop toward CW (the default).
    Ccw,  ///< Fill grows from the CW stop toward CCW (reverse level).
};

/* ── Compose mode ───────────────────────────────────────────────────────── */

enum class FillCompose : uint8_t
{
    Replace,  ///< Write all pixels (active and passive regions).
    Overlay,  ///< Write only active pixels; passive region untouched.
};

/* ── Fill descriptor ────────────────────────────────────────────────────── */

/**
 * All parameters for a DrawFill call.
 *
 * Interpretation by fill mode:
 *
 *   color        — active fill: Edge = fill color, Center = CW arm color.
 *   neg_color    — Center mode: CCW arm color (ignored by Edge).  Leave at
 *                  the default (transparent) to paint both arms in `color`.
 *   passive_color— passive region color (used in Replace compose mode).
 *   center_color — Center mode: pivot-step color (ignored by Edge).
 *   pivot01      — Center: fan origin in the control's 0..1 value space
 *                  (0.5 = the arc midpoint).  Each arm is normalized to
 *                  its own side, so both reach their ends at the pot
 *                  extremes regardless of where the pivot sits.  Named
 *                  for its unit: this is NOT an LED step index.
 *   direction    — Edge: which stop the fill grows from.
 *
 * Colors should be pre-scaled by the caller (e.g. catch-attenuation × global brightness).
 * DrawFill applies ScaleGlobal() internally before writing.
 */
struct FillDesc
{
    FillMode      mode          = FillMode::Edge;
    LedPanel::Rgb color         = {0xFF, 0xFF, 0xFF};  ///< Active fill / CW arm.
    /**
     * CCW arm color in Center mode.  Sentinel {0,0,0} means "same as `color`"
     * — preserves single-color behavior for callers that don't care about
     * per-arm tinting.  Set to a non-black RGB to paint the CCW arm in its
     * own color (e.g. attenuverter green/red, damping warm/cool).
     */
    LedPanel::Rgb neg_color     = {0u,   0u,   0u};
    LedPanel::Rgb passive_color = {0u,   0u,   0u};    ///< Passive trail (Replace mode).
    LedPanel::Rgb center_color  = {0xFF, 0xFF, 0xFF};  ///< Center pivot color (Center mode only).
    float         pivot01       = 0.5f;                 ///< Center fan origin, 0..1 value space.
    FillDirection direction     = FillDirection::Cw;    ///< Edge growth direction.
    FillAnim      anim          = FillAnim::None;        ///< Animation on the active region.
    float         anim_depth    = 0.4f;                  ///< Modulation depth 0..1.
    FillCompose   compose       = FillCompose::Replace;  ///< Compose mode.
};

/* ── Edge-snap utility ──────────────────────────────────────────────────── */

/** Dead-zone for the fractional fill edge: snaps tiny fractions to 0 or 1. */
constexpr float kArcEdgeSnap = 0.04f;

/**
 * Apply the edge-snap dead-zone to a decomposed fill value.
 *
 * Usage:
 *   float   f    = fill_v * num_steps;
 *   uint8_t hard = (uint8_t)f;
 *   float   frac = f - hard;
 *   ArcSnapFrac(hard, frac, num_steps);
 */
inline void ArcSnapFrac(uint8_t& fill_hard, float& fill_frac, uint8_t max_steps)
{
    if (fill_frac < kArcEdgeSnap)
    {
        fill_frac = 0.0f;
    }
    else if (fill_frac > 1.0f - kArcEdgeSnap)
    {
        if (fill_hard < max_steps)
            fill_hard = static_cast<uint8_t>(fill_hard + 1u);
        fill_frac = 0.0f;
    }
}

/* ── DrawFill ───────────────────────────────────────────────────────────── */

/**
 * Draw a filled arc on a pot ring.
 *
 * @param dst        Target LED panel.
 * @param pot_idx    Ring index (0-based).
 * @param start_hour Clock-face hour of the CCW arc endpoint.
 * @param step_hours Hours per arc step.
 * @param num_steps  Total arc steps.
 * @param value      Fill level: Edge 0..1 | Center -1..+1.
 * @param desc       Fill style, colors, and animation parameters.
 * @param t_ms       Absolute time in milliseconds (drives Pulse/Ripple animation).
 */
void DrawFill(LedPanel&       dst,
              uint8_t         pot_idx,
              float           start_hour,
              float           step_hours,
              uint8_t         num_steps,
              float           value,
              const FillDesc& desc,
              uint32_t        t_ms = 0u);

/** ArcGeometry overload — replaces the three geometry scalar arguments. */
inline void DrawFill(LedPanel&          dst,
                     uint8_t            pot_idx,
                     const ArcGeometry& geo,
                     float              value,
                     const FillDesc&    desc,
                     uint32_t           t_ms = 0u)
{
    DrawFill(dst, pot_idx, geo.start_hour, geo.step_hours, geo.arc_leds,
             value, desc, t_ms);
}

} // namespace alchemy
