/**
 * @file alchemy/surface/param_lock.h
 * @brief alchemy::ParamLock — opt-in looping-automation surface.
 *
 * A ParamLock surface owns an array of automation slots and the trigger
 * button's gesture state.  Each frame `Update()` advances every slot's
 * play head; `Delta(p)` reads the current modulation amount for pot p on
 * the visible page (resolved via an optional Pager) without advancing.
 *
 * TODO: Update to use Matthew's sparse sampling, which will be sick.
 * 
 * Two constructors, one template parameter (total slot count):
 *
 *   alchemy::ParamLock<6>  locks(hw.buttons[0]);            // unpaged, 6 slots
 *   alchemy::ParamLock<12> locks(hw.buttons[0], pager);     // paged, 12 = 2 × 6
 *
 * The paged form resolves Delta(p) as `pager.Page() * pager.NumPots() + p`.
 * The unpaged form uses `p` directly.  Advance() moves all kSlots play
 * heads each frame so automation on inactive pages keeps cycling; it is
 * separate from Update() (gestures) so playback survives modes — like the
 * Settings menu — that suspend gesture handling.
 *
 * Composition at the user's read site:
 *
 *   float knob(uint8_t p, void*) {
 *       return pager.Value(p) + locks.Delta(p) + cv.Delta(p, cv_buf, n);
 *   }
 *
 * Saving locks into a preset:
 *
 *   locks.Save(preset.locks);     // length kSlots
 *   locks.Restore(preset.locks);
 */

#pragma once

#include <cstdint>
#include <cstring>
#include "alchemy/control/pot_catch.h"
#include "alchemy/control/param_lock_manager.h"
#include "alchemy/hw/i_button.h"
#include "alchemy/surface/lock_source.h"
#include "alchemy/surface/pager.h"
#include "alchemy/surface/serializable.h"

namespace alchemy {

class LedPanel;

/* Schema tag for ParamLock — bump if SavedEntry layout changes. */
inline constexpr uint32_t kParamLockSchemaTag = 0x504C4B30u; /* 'PLK0' */

template<uint8_t kSlots>
class ParamLock : public Serializable, public LockSource
{
  public:
    using SavedEntry = ParamLockManager::SavedEntry;
    static constexpr uint8_t kNumSlots = kSlots;

    /** Unpaged form: kSlots is the pot count, Delta(p) reads slot p. */
    explicit ParamLock(IButton& trigger)
        : trigger_(&trigger), pager_(nullptr)
    {
        mgr_.Init(slots_, kSlots, kSlots);
    }

    /**
     * Paged form: kSlots must equal pager.NumPages() * pager.NumPots().
     * Delta(p) reads slot (pager.Page() * pager.NumPots() + p).
     */
    ParamLock(IButton& trigger, Pager& pgr)
        : trigger_(&trigger), pager_(&pgr)
    {
        mgr_.Init(slots_, kSlots, pgr.NumPots());
    }

    /**
     * Process one frame of GESTURE state: consume the trigger edges
     * PollButtons detected and run the record/disarm gesture if held.
     *
     * Playback is NOT advanced here — that's Advance()'s job, split out
     * so recorded modulation keeps running in modes (e.g. the Settings
     * menu) that suspend gesture handling.  ControlLoop calls both;
     * standalone users must now call Advance() every frame themselves
     * in addition to Update().
     *
     * **Call order (paged form).**  When constructed with a `Pager&`,
     * this method calls `pager.ConsumeButton()` on the trigger's
     * falling edge if a hold-and-nudge gesture armed a lock — so the
     * B1 release that ended the hold does not also advance the page.
     * For that suppression to land in the right frame, `Update()` MUST
     * be called **before** `Pager::Update()`:
     *
     * ```cpp
     * locks.Update(phys, now);   // ← runs first; may set pager.ConsumeButton()
     * pager.Update(phys, now);   // ← reads the consume flag this frame
     * ```
     *
     * Reverse the order and the pager has already processed the
     * release this frame; the consume flag latches to the next,
     * unrelated B1 release.  See `Pager::Update` for the full contract.
     */
    void PollButtons(uint32_t t_ms, bool gated) override;
    void Update     (const float* phys, uint32_t /*t_ms*/) override;

    /** Advance every active slot's play head by one frame.  Touches no
     *  gesture state — safe (and required) every frame, even while a
     *  mode like Settings suspends Update(). */
    void Advance() override;

    /** Modulation delta for pot @p p on the visible page (no advance). */
    float Delta(uint8_t pot) const;

    /** True if a slot is actively playing back (current page, or unpaged). */
    bool IsActive(uint8_t pot) const;

    /** True if a slot is currently recording. */
    bool IsRecording(uint8_t pot) const;

    /** Read the modulation delta for an arbitrary (page, pot) slot, ignoring
     *  the pager's current page.  For composing inactive-page parameters. */
    float DeltaAtPage(uint8_t page, uint8_t pot) const override;

    /** True if the (page, pot) slot is actively playing back. */
    bool IsActiveAtPage(uint8_t page, uint8_t pot) const override;

    /** True if the (page, pot) slot is currently recording. */
    bool IsRecordingAtPage(uint8_t page, uint8_t pot) const override;

    /** True if any slot on the visible page is recording. */
    bool AnyRecording() const;

    /** True if any slot on the visible page is active. */
    bool AnyActive() const;

    /**
     * Optional default overlay: marks rings whose lock is active/recording with
     * a colored pip at 6 o'clock.  Recording = red, active = green.
     */
    void Render(LedPanel& panel, uint32_t t_ms) const override;

    /** Serialise every slot into @p out[0..kSlots-1]. */
    void Save(SavedEntry out[]) const { mgr_.Capture(out, kSlots, 0); }

    /** Restore every slot from @p in[0..kSlots-1]. */
    void Restore(const SavedEntry in[]) { mgr_.Restore(in, kSlots, 0); }

    /** Reset every slot to its default (inactive) state. */
    void Clear() { mgr_.Clear(); }

    /** True while the trigger button is held (between rising and falling). */
    bool IsButtonHeld() const { return mgr_.IsButtonHeld(); }

    /* ── Serializable ──────────────────────────────────────────────────
     * Round-trips the full SavedEntry array; identical to the existing
     * Save()/Restore() but exposed through the Serializable contract so
     * Presets can manage this object directly.
     *
     * Staged ONE entry at a time: a SavedEntry is ~1 KiB (256-sample
     * motion buffer), so a whole-array stack local would spike
     * kSlots × ~1 KiB deep inside the control loop (HostLink live
     * writes, GET_LIVE, panel save/load).  The byte stream is unchanged
     * — same entries, same order.                                    */

    size_t SerializedSize() const override
    {
        return static_cast<size_t>(kSlots) * sizeof(SavedEntry);
    }

    void Serialize(uint8_t* out) const override
    {
        SavedEntry e;
        for (uint8_t i = 0; i < kSlots; i++, out += sizeof(e))
        {
            mgr_.Capture(&e, 1, i);
            std::memcpy(out, &e, sizeof(e));
        }
    }

    bool Deserialize(const uint8_t* in) override
    {
        SavedEntry e;
        for (uint8_t i = 0; i < kSlots; i++, in += sizeof(e))
        {
            std::memcpy(&e, in, sizeof(e));
            mgr_.Restore(&e, 1, i);
        }
        return true;
    }

    uint32_t SchemaHash() const override
    {
        return kParamLockSchemaTag
             ^ (static_cast<uint32_t>(kSlots) << 8)
             ^ static_cast<uint32_t>(sizeof(SavedEntry));
    }

  private:
    IButton*         trigger_;
    Pager*           pager_;
    bool             prev_pressed_    = false;
    bool             rising_pending_  = false;
    bool             falling_pending_ = false;
    ParamLockSlot    slots_[kSlots] = {};
    ParamLockManager mgr_;

    uint8_t Offset() const { return pager_ ? pager_->Page() * pager_->NumPots() : 0u; }
    uint8_t Width()  const { return pager_ ? pager_->NumPots() : kSlots; }
};

} // namespace alchemy

#include "alchemy/surface/param_lock_impl.h"
