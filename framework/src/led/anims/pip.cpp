/**
 * @file pip.cpp
 *
 * Thin wrapper: the pip algorithm lives in RingFrame::Pip.  This entry
 * point keeps the panel-direct signature (full-arc position, overlay
 * compositing — only the pip's own LEDs are written unless a background
 * is set).
 */

#include "alchemy/led/anims/pip.h"
#include "alchemy/led/ring_frame.h"

namespace alchemy {

void DrawPip(LedPanel&      dst,
             uint8_t        pot_idx,
             float          start_hour,
             float          step_hours,
             uint8_t        arc_leds,
             float          value,
             const PipDesc& desc,
             uint32_t       t_ms)
{
    if (arc_leds == 0u) return;
    const ArcGeometry geo{start_hour, step_hours, arc_leds};

    RingFrame f;
    f.BeginOverlay(geo);
    f.Pip(Region::Full, desc, value, 1.0f, t_ms);
    f.Emit(dst, pot_idx);
}

} // namespace alchemy
