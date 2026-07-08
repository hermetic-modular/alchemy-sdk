/**
 * @file alchemy/surface/lock_source.h
 * @brief alchemy::LockSource — abstract automation-delta source for VirtualKnob.
 *
 * VirtualKnob asks its attached LockSource for the lock delta at a specific
 * (page, pot) cell. ParamLock<N> implements this; users with custom
 * automation systems can implement their own.
 *
 * Read paths must be ISR-safe and cheap — VirtualKnob's .Value() / .Norm()
 * routes through DeltaAtPage() and may be called from audio.
 */

#pragma once

#include <cstdint>

namespace alchemy {

class LedPanel;

class LockSource
{
  public:
    virtual ~LockSource() = default;

    /** Modulation delta for one (page, pot) slot. ISR-safe; cheap. */
    virtual float DeltaAtPage(uint8_t page, uint8_t pot) const = 0;

    /** True if the (page, pot) slot is recording. */
    virtual bool  IsRecordingAtPage(uint8_t page, uint8_t pot) const = 0;

    /** True if the (page, pot) slot is actively playing back. */
    virtual bool  IsActiveAtPage(uint8_t page, uint8_t pot) const = 0;

    /**
     * Detect trigger-button edges at 1 ms cadence. Called from ControlLoop's
     * inner poll loop. Sets internal pending flags consumed by Update().
     * When @p gated, sync prev-pressed state but don't fire actions.
     *
     * Default: no-op.
     */
    virtual void  PollButtons(uint32_t /*t_ms*/, bool /*gated*/) {}

    /**
     * Process one control-loop frame of gesture state (record/disarm).
     * Main thread only.  Gated by ControlLoop while Settings is active —
     * playback must NOT live here; that's Advance()'s job.
     */
    virtual void  Update(const float* phys, uint32_t t_ms) = 0;

    /**
     * Advance playback by one frame.  Called by ControlLoop every frame,
     * unconditionally — recorded automation keeps running while Settings
     * owns the surface.  Touches no gesture state.
     *
     * Default: no-op (custom sources that advance elsewhere need nothing).
     */
    virtual void  Advance() {}

    /**
     * Paint automation-state overlay (e.g. recording/active pips) onto the
     * panel. Called by ControlLoop after the standard render. Default no-op.
     */
    virtual void  Render(LedPanel& /*panel*/, uint32_t /*t_ms*/) const {}
};

} // namespace alchemy
