/**
 * @file alchemy/surface/knob_storage.h
 * @brief alchemy::KnobStorage — abstract storage backend for VirtualKnob.
 *
 * VirtualKnob does not talk to Pager directly. Instead, it holds a
 * KnobStorage* and a page index, both wired at attach time by ControlLoop.
 * Pager is the canonical implementation; users who want custom page
 * mechanics or pot-catch behavior implement their own type satisfying
 * this interface.
 *
 * Required:
 *   - Stored(page, pot)  : catch-aware stored value for a (page, pot) cell.
 *   - ActivePage()       : which page is currently visible.
 *   - Update(phys, t_ms) : process one frame; called by ControlLoop.
 *   - NumPages / NumPots : bounds, used at attach time.
 *
 * Optional:
 *   - LockPage(page, phys) : re-arm catch for an entire page (no-op default).
 *   - PageColor(page)      : per-page indicator color (returns {0,0,0} default).
 *
 * Read paths (Stored, ActivePage) must be ISR-safe and cheap — VirtualKnob's
 * .Value() / .Norm() routes through Stored() and may be called from audio.
 */

#pragma once

#include <cstdint>
#include "alchemy/control/pot_catch.h"   /* PotState */
#include "alchemy/led/panel.h"           /* LedPanel::Rgb */

namespace alchemy {

class IButton;

class KnobStorage
{
  public:
    virtual ~KnobStorage() = default;

    /** Catch-aware stored value for (page, pot). ISR-safe; cheap. */
    virtual float   Stored(uint8_t page, uint8_t pot) const = 0;

    /** Currently visible page. ISR-safe; cheap. */
    virtual uint8_t ActivePage() const = 0;

    /** Pots-per-page count (uniform across pages). */
    virtual uint8_t NumPots()   const = 0;

    /** Page count (≥ 1). */
    virtual uint8_t NumPages()  const = 0;

    /**
     * Detect button edges at 1 ms cadence. Called from ControlLoop's inner
     * poll loop. Implementations should be edge-detection only — set flags
     * to be processed by the next Update(). When @p gated is true, sync
     * any internal prev-pressed state but do NOT fire actions (e.g.
     * Settings is active and owns the buttons).
     *
     * Default: no-op.
     */
    virtual void    PollButtons(uint32_t /*t_ms*/, bool /*gated*/) {}

    /** Process one control-loop frame. Main thread only. */
    virtual void    Update(const float* phys, uint32_t t_ms) = 0;

    /**
     * Full PotState for (page, pot) — includes the catch flag used by the
     * renderer to dim uncaught rings. Default: stored value, always caught.
     */
    virtual PotState State(uint8_t page, uint8_t pot) const
    {
        return PotState{Stored(page, pot), true, 0};
    }

    /** Re-arm catch for every pot on @p page (e.g. after preset load). */
    virtual void    LockPage(uint8_t /*page*/, const float* /*phys*/) {}

    /**
     * Per-page indicator color, painted by ControlLoop on the page-advance
     * button when this is the active page. Default: black (no indicator).
     */
    virtual LedPanel::Rgb PageColor(uint8_t /*page*/) const { return {0, 0, 0}; }

    /**
     * The physical button this storage's page-advance gesture consumes,
     * or nullptr when it has none.  Surfaces that claim a gesture on the
     * same button (ParamLock, ButtonBank) compare against this and call
     * ConsumeButton() so the same release doesn't also advance the page.
     */
    virtual const IButton* PageButton() const { return nullptr; }

    /** Suppress the next page-advance release (no-op by default). */
    virtual void ConsumeButton() {}
};

} // namespace alchemy
