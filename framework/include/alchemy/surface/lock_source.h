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

    /* ── Sync-mode binding (Settings::UseLocks) ──────────────────────
     * Free/Clocked as a raw byte so Settings can push a selector index
     * without knowing the concrete lock type.  Defaults are no-ops so
     * custom automation sources are unaffected. */

    /** Set the sync mode (0 = free, 1 = clocked; see alchemy::LockSync). */
    virtual void    SetSyncMode(uint8_t /*mode*/) {}

    /** Current sync mode as a selector index. */
    virtual uint8_t SyncMode() const { return 0u; }

    /** True when a musical clock is wired (Clocked mode is meaningful).
     *  Default true so custom sources aren't nagged by UseLocks' guard. */
    virtual bool    HasClock() const { return true; }

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
