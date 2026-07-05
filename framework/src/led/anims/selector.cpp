/**
 * @file selector.cpp
 *
 * Thin wrapper: the selector algorithm lives in RingFrame::BaseZone.
 * This entry point keeps the panel-direct signature.
 */

#include "alchemy/led/anims/selector.h"
#include "alchemy/led/ring_frame.h"

namespace alchemy {

void DrawSelector(LedPanel&           dst,
                  uint8_t             pot_idx,
                  float               start_hour,
                  float               step_hours,
                  uint8_t             arc_leds,
                  float               value,
                  const SelectorDesc& desc)
{
    if (arc_leds == 0u || desc.num_zones == 0u) return;
    const ArcGeometry geo{start_hour, step_hours, arc_leds};

    RingFrame f;
    f.Begin(geo);
    f.Base(desc, value);
    f.Emit(dst, pot_idx);
}

} // namespace alchemy
