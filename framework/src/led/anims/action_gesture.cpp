/**
 * @file action_gesture.cpp
 */

#include "alchemy/led/anims/action_gesture.h"
#include "alchemy/led/anims/fill.h"

namespace alchemy {

void DrawActionGesture(LedPanel&                 dst,
                       uint8_t                   pot_idx,
                       float                     arc_start_hour,
                       float                     arc_step_hours,
                       uint8_t                   arc_leds,
                       uint8_t                   phase,
                       float                     hold_ms_elapsed,
                       float                     flash_ms_elapsed,
                       const PotState&           s,
                       float                     fill_k,
                       const ActionGestureStyle& style)
{
    dst.ClearRing(pot_idx);

    if (phase == static_cast<uint8_t>(ActionGesturePhase::FlashConfirm))
    {
        const uint32_t f  = static_cast<uint32_t>(flash_ms_elapsed);
        const bool     on = ((f / static_cast<uint32_t>(style.flash_half_ms)) & 1u) == 0u;
        if (on)
        {
            for (uint8_t off = 0; off < style.ring_leds; off++)
                dst.SetRingByOffset(pot_idx, off,
                    dst.ScaleGlobal(LedPanel::Scale(style.confirm_color, fill_k)));
        }
        return;
    }

    if (phase == static_cast<uint8_t>(ActionGesturePhase::Saving) ||
        phase == static_cast<uint8_t>(ActionGesturePhase::Loading))
    {
        float prog = hold_ms_elapsed / style.hold_ms;
        if (prog < 0.0f) prog = 0.0f;
        if (prog > 1.0f) prog = 1.0f;
        const uint8_t fill = static_cast<uint8_t>(
            prog * static_cast<float>(arc_leds) + 0.5f);

        for (uint8_t step = 0; step < arc_leds; step++)
        {
            const bool lit = (phase == static_cast<uint8_t>(ActionGesturePhase::Saving))
                                 ? (step < fill)
                                 : (step >= (arc_leds - fill));
            if (!lit) continue;
            const float   hour = arc_start_hour + static_cast<float>(step) * arc_step_hours;
            const uint8_t off  = dst.RingOffsetForHour(pot_idx, hour);
            dst.SetRingByOffset(pot_idx, off,
                dst.ScaleGlobal(LedPanel::Scale(style.progress_color, fill_k)));
        }
        return;
    }

    if (phase == static_cast<uint8_t>(ActionGesturePhase::Disarmed))
        return;

    /* Idle: bipolar proximity cue — shows which end the pot is near. */
    const float signed_v = (s.stored - 0.5f) * 2.0f;
    FillDesc idle_desc;
    idle_desc.mode         = FillMode::Center;
    idle_desc.color        = LedPanel::Scale(style.progress_color, fill_k);
    idle_desc.passive_color= LedPanel::Scale(style.progress_color, fill_k);
    idle_desc.center_color = LedPanel::Scale(style.confirm_color,  fill_k);
    idle_desc.pivot01      = 0.5f;
    DrawFill(dst, pot_idx, arc_start_hour, arc_step_hours, arc_leds, signed_v, idle_desc);
}

} // namespace alchemy
