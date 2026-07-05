/**
 * @file fill.cpp
 *
 * Thin wrapper: the fill algorithm lives in RingFrame::Base(FillDesc).
 * This entry point keeps the panel-direct signature and the legacy
 * Center value convention (−1..+1 around the arc midpoint).
 */

#include "alchemy/led/anims/fill.h"
#include "alchemy/led/ring_frame.h"

namespace alchemy {

void DrawFill(LedPanel&       dst,
              uint8_t         pot_idx,
              float           start_hour,
              float           step_hours,
              uint8_t         num_steps,
              float           value,
              const FillDesc& desc,
              uint32_t        t_ms)
{
    const ArcGeometry geo{start_hour, step_hours, num_steps};

    float v01 = value;
    if (desc.mode == FillMode::Center)
    {
        if (v01 < -1.0f) v01 = -1.0f;
        if (v01 >  1.0f) v01 =  1.0f;
        v01 = 0.5f + 0.5f * v01;
    }

    RingFrame f;
    if (desc.compose == FillCompose::Overlay)
        f.BeginOverlay(geo);
    else
        f.Begin(geo);
    f.Base(desc, v01, t_ms);
    f.Emit(dst, pot_idx);
}

} // namespace alchemy
