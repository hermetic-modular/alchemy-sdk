/**
 * @file perf_renderer.cpp
 * @brief alchemy::PerfRenderer implementation.
 */

#include "alchemy/led/perf_renderer.h"
#include "alchemy/led/animations.h"
#include "alchemy/led/ring_frame.h"
#include "alchemy/led/panel.h"

namespace alchemy {

void PerfRenderer::Render(LedPanel&        dst,
                          const ParamSlot* slots,
                          const PotState*  pots,
                          const float*     combined,
                          uint8_t          num_pots,
                          uint32_t         t_ms) const
{
    const ArcGeometry geo{arc_start_hour_, arc_step_hours_, arc_leds_};

    for (uint8_t p = 0; p < num_pots; ++p)
    {
        const ParamSlot& slot = slots[p];

        if (slot.arc_style == ArcStyle::None)
            continue;

        const PotState& ps     = pots[p];
        const float     cv     = combined[p];
        const float     fill_k = ps.caught ? 1.0f : 0.30f;

        /* Captured by the Gradient arc case, read by the GradientSnap pip. */
        ColorMorphResult morph_result = {{0u, 0u, 0u}, false};

        /* ── Arc ─────────────────────────────────────────────────── */

        RingFrame f;
        f.Begin(geo);
        bool frame_used = true;

        switch (slot.arc_style)
        {
            case ArcStyle::Level:
            {
                FillDesc desc      = slot.fill;
                desc.mode          = FillMode::Edge;
                desc.color         = LedPanel::Scale(desc.color,         fill_k);
                desc.passive_color = LedPanel::Scale(desc.passive_color, fill_k);
                f.Base(desc, cv, t_ms);
                break;
            }

            case ArcStyle::Bipolar:
            {
                FillDesc desc     = slot.fill;
                desc.mode         = FillMode::Center;
                desc.color        = LedPanel::Scale(desc.color,        fill_k);
                desc.neg_color    = LedPanel::Scale(desc.neg_color,    fill_k);
                desc.center_color = LedPanel::Scale(desc.center_color, fill_k);
                f.Base(desc, cv, t_ms);
                break;
            }

            case ArcStyle::Selector:
            {
                SelectorDesc sel   = slot.selector;
                sel.active_color   = LedPanel::Scale(sel.active_color,   fill_k);
                sel.inactive_color = LedPanel::Scale(sel.inactive_color, fill_k);
                f.Base(sel, cv);
                break;
            }

            case ArcStyle::Gradient:
            {
                frame_used = false;
                if (slot.gradient.snaps && slot.gradient.num_snaps > 0u)
                {
                    morph_result = DrawColorMorphArc(
                        dst, p,
                        arc_start_hour_, arc_step_hours_, arc_leds_,
                        cv, ps.caught,
                        slot.gradient.snaps, slot.gradient.num_snaps);
                }
                break;
            }

            case ArcStyle::GradientFill:
            {
                /* Plain Edge fill whose color is sampled from a sibling pot's
                 * value through the snap gradient; falls back to fill.color. */
                LedPanel::Rgb  col = slot.fill.color;
                const uint8_t  src = slot.gradient.color_src_pot;
                if (src < num_pots && slot.gradient.snaps
                    && slot.gradient.num_snaps > 0u)
                    col = ColorMorphColor(combined[src],
                                          slot.gradient.snaps,
                                          slot.gradient.num_snaps);

                FillDesc desc      = slot.fill;
                desc.mode          = FillMode::Edge;
                desc.color         = LedPanel::Scale(col, fill_k);
                desc.passive_color = LedPanel::Scale(desc.passive_color, fill_k);
                f.Base(desc, cv, t_ms);
                break;
            }

            default:
                frame_used = false;
                break;
        }

        if (frame_used)
            f.Emit(dst, p);

        /* ── Bottom pip ──────────────────────────────────────────── */

        const uint8_t bottom = dst.RingOffsetForHour(p, 6.0f);

        switch (slot.pip.style)
        {
            case PipStyle::Solid:
                dst.SetRingByOffset(p, bottom,
                    dst.ScaleGlobal(LedPanel::Scale(slot.pip.color, fill_k)));
                break;

            case PipStyle::ThresholdSnap:
                if (cv >= slot.pip.snap_lo && cv <= slot.pip.snap_hi)
                    dst.SetRingByOffset(p, bottom,
                        dst.ScaleGlobal(LedPanel::Scale(slot.pip.color, fill_k)));
                else if (cv > slot.pip.snap_hi)
                    dst.SetRingByOffset(p, bottom,
                        dst.ScaleGlobal(LedPanel::Scale(slot.pip.snap_hi_color, fill_k)));
                break;

            case PipStyle::GradientSnap:
                if (morph_result.at_snap && ps.caught)
                    dst.SetRingByOffset(p, bottom,
                        dst.ScaleGlobal(morph_result.fill_color));
                break;

            case PipStyle::None:
            default:
                break;
        }

        /* ── Catch pip ───────────────────────────────────────────── */

        DrawCatchPip(dst, p, ps, arc_start_hour_, arc_step_hours_, arc_leds_, {0xFF, 0xFF, 0xFF});
    }
}

} // namespace alchemy
