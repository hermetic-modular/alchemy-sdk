/**
 * @file level_arc.h
 * @brief VU-style level meter arc drawing.
 *
 * DrawLevelArc renders a signal level bar with configurable low/mid/high
 * color zones.  It only writes pixels within the current fill range,
 * leaving pixels beyond untouched — so it can be layered on top of a
 * base arc (e.g. gain fill) without clobbering it.
 *
 * LevelColor computes the zone color for a given level without drawing.
 * Use it to get the right color for a DrawFill call when you want to
 * combine level coloring with the full DrawFill feature set.
 *
 * DrawLevelArc is now an inline wrapper around DrawFill with
 * FillCompose::Overlay — zero-overhead, no separate compilation unit.
 */

#pragma once

#include <cstdint>
#include "alchemy/hardware_types.h"
#include "alchemy/led/panel.h"
#include "alchemy/led/anims/fill.h"

namespace alchemy {

/** Normalized level thresholds for DrawLevelArc color zones. */
struct LevelArcThresholds
{
    float mid_start;   ///< Level (0..1) at which color transitions to mid.
    float high_start;  ///< Level (0..1) at which color transitions to high.
};

/**
 * Compute the zone color for a given level without drawing.
 *
 * Returns low_color when level < zones.mid_start,
 *         mid_color when level < zones.high_start,
 *         high_color otherwise.
 */
inline LedPanel::Rgb LevelColor(float              level,
                                LevelArcThresholds zones,
                                LedPanel::Rgb      low_color,
                                LedPanel::Rgb      mid_color,
                                LedPanel::Rgb      high_color)
{
    if (level >= zones.high_start) return high_color;
    if (level >= zones.mid_start)  return mid_color;
    return low_color;
}

/**
 * Draw a VU-style level bar with low / mid / high color zones.
 *
 * Only pixels within the current level are written; pixels beyond are left
 * unchanged, allowing a base arc to show through.
 *
 * Global brightness is applied internally.  Colors should NOT be
 * pre-scaled by the caller.
 *
 * @param dst          Target LED panel.
 * @param pot_idx      Ring index.
 * @param start_hour   Clock-face hour of the CCW endpoint.
 * @param step_hours   Hours per arc step.
 * @param num_steps    Total arc steps.
 * @param level_norm   Signal level 0..1.
 * @param zones        Mid and high transition thresholds.
 * @param low_color    Color for the low zone.
 * @param mid_color    Color for the mid zone.
 * @param high_color   Color for the high zone.
 */
inline void DrawLevelArc(LedPanel&          dst,
                         uint8_t            pot_idx,
                         float              start_hour,
                         float              step_hours,
                         uint8_t            num_steps,
                         float              level_norm,
                         LevelArcThresholds zones,
                         LedPanel::Rgb      low_color,
                         LedPanel::Rgb      mid_color,
                         LedPanel::Rgb      high_color)
{
    FillDesc desc;
    desc.mode    = FillMode::Edge;
    desc.color   = LevelColor(level_norm, zones, low_color, mid_color, high_color);
    desc.compose = FillCompose::Overlay;
    DrawFill(dst, pot_idx, start_hour, step_hours, num_steps, level_norm, desc);
}

/** ArcGeometry overload. */
inline void DrawLevelArc(LedPanel&          dst,
                         uint8_t            pot_idx,
                         const ArcGeometry& geo,
                         float              level_norm,
                         LevelArcThresholds zones,
                         LedPanel::Rgb      low_color,
                         LedPanel::Rgb      mid_color,
                         LedPanel::Rgb      high_color)
{
    DrawLevelArc(dst, pot_idx,
                 geo.start_hour, geo.step_hours, geo.arc_leds,
                 level_norm, zones, low_color, mid_color, high_color);
}

} // namespace alchemy
