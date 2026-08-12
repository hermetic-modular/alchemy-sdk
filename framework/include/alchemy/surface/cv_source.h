/**
 * @file alchemy/surface/cv_source.h
 * @brief alchemy::CvSource — abstract per-(page, pot) CV-delta source.
 *
 * VirtualKnob's static `.Cv(idx, atten, bipolar)` binding pins one CV channel
 * to one knob at compile time.  Some modules need the mapping to be chosen
 * at runtime (a settings page routes "CV3 -> Delay1 feedback" today, "CV3 ->
 * Delay2 mix" tomorrow), or to dispatch the same CV channel to non-knob
 * targets (a clock tap, a freeze gate).  Those modules attach a CvSource to
 * the ControlLoop instead.
 *
 * When a CvSource is attached:
 *   - VirtualKnob.Norm() / .Value() consult `source->DeltaAtPage(page, pot)`
 *     instead of the static binding for every knob the loop wired.
 *   - ControlLoop calls `PollEdges(cv, t_us)` from the 1 ms inner poll
 *     loop (sub-frame edge timestamps).
 *   - ControlLoop calls `Update(cv, t_ms)` once per frame
 *
 * The interface is intentionally symmetric with LockSource — the two
 * surfaces compose at the read site as
 * `Stored() + LockSource::DeltaAtPage() + CvSource::DeltaAtPage()`.
 *
 * Read paths must be ISR-safe and cheap — VirtualKnob's .Value() / .Norm()
 * routes through DeltaAtPage() and may be called from audio.
 */

#pragma once

#include <cstdint>

namespace alchemy {

class CvSource
{
  public:
    virtual ~CvSource() = default;

    /**
     * Composed CV modulation delta for one (page, pot) slot, this frame.
     * ISR-safe; cheap.  Implementations typically cache a per-(page, pot)
     * sum during Update() and return it here without recomputing.
     */
    virtual float DeltaAtPage(uint8_t page, uint8_t pot) const = 0;

    /**
     * Detect CV-edge events at 1 ms cadence.  Called from ControlLoop's
     * inner poll loop with the current cv[] snapshot and a microsecond
     * timestamp for sub-frame accuracy on clock-tap / gate destinations.
     *
     * Default: no-op.
     */
    virtual void  PollEdges(const float* /*cv*/,
                            uint32_t     /*t_us*/) {}

    /**
     * Process one control-loop frame.  Implementations recompute their
     * per-(page, pot) delta cache from the latest cv[] and any cached
     * routing tables.  Main thread only.
     */
    virtual void  Update(const float* cv, uint32_t t_ms) = 0;
};

} // namespace alchemy
