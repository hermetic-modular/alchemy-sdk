/**
 * @file alchemy/led/anim_desc.h
 * @brief LED ring animation style enumerations + ParamSlot descriptor.
 *
 * `ArcStyle` and `PipStyle` are used by `ParamSlot` (declared here) to
 * describe how each pot ring should be rendered by `PerfRenderer`.  The
 * developer populates a `ParamSlot[]` (or uses the `LedRender::Binder`
 * fluent wrapper) and `PerfRenderer` walks the array each frame.
 *
 * ParamSlot embeds the same descriptors the drawing primitives consume
 * (`FillDesc`, `SelectorDesc`) rather than flattened copies of their
 * fields, so the declarative path and custom RingFrame rendering share
 * one vocabulary.  `arc_style` selects which embedded descriptor the
 * renderer reads:
 *
 *   Level / Bipolar / GradientFill -> fill      (mode is set by the style)
 *   Selector                       -> selector
 *   Gradient / GradientFill        -> gradient  (snap points)
 *
 * When arc_style is ArcStyle::None the renderer skips that pot entirely,
 * leaving the developer free to render it manually using RingFrame and
 * the primitives in alchemy/led/animations.h.
 */

#pragma once

#include <cstdint>
#include "alchemy/led/panel.h"
#include "alchemy/led/anims/fill.h"             /* FillDesc, FillAnim, FillMode */
#include "alchemy/led/anims/selector.h"         /* SelectorDesc, ZoneGeometry */
#include "alchemy/led/anims/color_morph_arc.h"  /* MorphSnapPoint */

namespace alchemy {

/**
 * How to draw the arc portion of a pot ring.
 *
 *  Level    — filled arc from the CCW stop toward CW.  The standard
 *             unipolar control (time, mix, depth, …).
 *  Bipolar  — fan from a pivot outward in two arms.  Use for bipolar
 *             controls (damping, attenuverter, …).  The pivot defaults
 *             to the arc midpoint; set fill.pivot01 to move it.
 *  Selector — discrete zone indicator driven by DrawSelector().
 *  Gradient — color-morphing filled arc with snap-point markers.  Use
 *             for model-morph / waveform-select / filter-type controls
 *             that interpolate between a handful of named positions.
 *  GradientFill — plain Level fill whose color is sampled from a sibling
 *             pot's value via the snap-point gradient
 *             (gradient.color_src_pot).  Use for a depth/amount control
 *             tinted by a companion morph/model control on the same voice.
 *  None     — developer handles this pot's arc completely.
 */
enum class ArcStyle : uint8_t
{
    None,
    Level,
    Bipolar,
    Selector,
    Gradient,
    GradientFill,
};

/**
 * How to draw the bottom-pip of a pot ring.
 *
 *  None           — no pip.
 *  Solid          — always-on pip in pip.color.
 *  ThresholdSnap  — pip.color when value is in [snap_lo, snap_hi];
 *                   snap_hi_color when value exceeds snap_hi;
 *                   off below snap_lo.
 *  GradientSnap   — for a Gradient arc: lit in the morph fill color when the
 *                   value sits on a snap point and the pot is caught; off
 *                   otherwise.  A snap-confirmation cue for model-morph pots.
 */
enum class PipStyle : uint8_t
{
    None,
    Solid,
    ThresholdSnap,
    GradientSnap,
};

/** Snap-point gradient configuration (Gradient / GradientFill styles).
 *  Caller owns the snap-point storage; the array must outlive every
 *  Render() call that consults this slot. */
struct GradientDesc
{
    const MorphSnapPoint* snaps         = nullptr;
    uint8_t               num_snaps     = 0u;
    /** GradientFill: pot index whose value selects the fill color through
     *  the snaps.  0xFF means "no source" — the fill falls back to
     *  fill.color. */
    uint8_t               color_src_pot = 0xFFu;
};

/** Bottom-pip configuration. */
struct BottomPipDesc
{
    PipStyle      style         = PipStyle::None;
    LedPanel::Rgb color         = {0xFF, 0xFF, 0xFF};
    float         snap_lo       = 0.90f;
    float         snap_hi       = 0.92f;
    LedPanel::Rgb snap_hi_color = {0xFF, 0x00, 0x00};
};

/* Declarative-path defaults.  These match the flattened fields ParamSlot
 * had before it embedded the descriptors (mid-gray arc, 8 zones, inactive
 * zones dimmed to off), so binder code that sets only some fields renders
 * exactly as it did.  The raw primitives keep their own defaults. */

constexpr FillDesc SlotDefaultFill()
{
    FillDesc d;
    d.color        = {0x80, 0x80, 0x80};
    d.center_color = {0x80, 0x80, 0x80};
    return d;
}

constexpr SelectorDesc SlotDefaultSelector()
{
    SelectorDesc d;
    d.num_zones    = 8u;
    d.active_color = {0x80, 0x80, 0x80};
    d.inactive_dim = 0.0f;
    return d;
}

/**
 * Per-pot arc descriptor consumed by PerfRenderer.
 *
 * The renderer treats arc_style == None as "skip this slot entirely" so the
 * developer can take it over.
 */
struct ParamSlot
{
    ArcStyle      arc_style = ArcStyle::None;
    FillDesc      fill      = SlotDefaultFill();      ///< Level / Bipolar / GradientFill styling.
    SelectorDesc  selector  = SlotDefaultSelector();  ///< Selector styling.
    GradientDesc  gradient;                           ///< Gradient snap points.
    BottomPipDesc pip;                                ///< Bottom-pip styling.
};

} // namespace alchemy
