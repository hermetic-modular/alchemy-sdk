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


    virtual void  PollButtons(uint32_t /*t_ms*/, bool /*gated*/) {}

    virtual void  Update(const float* phys, uint32_t t_ms) = 0;

    virtual void  Advance() {}
    virtual void  SetFrameMs(uint32_t /*frame_ms*/) {}

    /**
     * Paint automation-state overlay (e.g. recording/active pips) onto the
     * panel. Called by ControlLoop after the standard render. Default no-op.
     */
    virtual void  Render(LedPanel& /*panel*/, uint32_t /*t_ms*/) const {}
};

} // namespace alchemy
