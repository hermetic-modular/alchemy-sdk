/**
 * @file alchemy/surface/pager.h
 * @brief alchemy::Pager — opt-in performance pagination surface.
 *
 * A Pager owns a page index and per-page pot-catch state.  It consumes one
 * button (the page-advance gesture) and an external array of physical pot
 * readings; everything else is composition at the user's call site.
 *
 * Pages + locks: pass `pager` into a `ParamLock<kSlots>` constructor and the
 * lock surface will resolve `Delta(p)` against `pager.Page()` automatically.
 */

#pragma once

#include <cstdint>
#include "alchemy/control/pot_catch.h"
#include "alchemy/hw/i_button.h"
#include "alchemy/led/panel.h"
#include "alchemy/surface/knob_storage.h"
#include "alchemy/surface/serializable.h"

namespace alchemy {

class Pager : public Serializable, public KnobStorage
{
  public:
    static constexpr uint8_t kMaxPages = 8u;
    static constexpr uint8_t kMaxPots  = 8u;

    /**
     * @param b1         B1 (or any button) that drives the page-advance gesture.
     * @param num_pages  Number of pages (1..kMaxPages).  Must be ≥ 1.
     * @param num_pots   Pots per page (1..kMaxPots).
     */
    Pager(IButton& b1, uint8_t num_pages, uint8_t num_pots);

    /**
     * Process one frame: poll the button, advance the page on clean release,
     * and run pot-catch on the active page.
     *
     * If a page advance occurs, the new page is locked (pots stay uncaught
     * until the user moves through their stored values).
     *
     * If any other surface in this frame consumes B1 by
     * calling `Pager::ConsumeButton()` — `ParamLock`, a freeze/chord
     * handler, a tap-tempo handler, etc. — those surfaces' `Update()`
     * methods MUST run **before** `Pager::Update()` so the consume flag
     * is set in the same frame as the release the pager is about to
     * process.  Reverse the order and the pager has already advanced
     * the page this frame; the consume flag then suppresses the *next*,
     * unrelated B1 release.  The canonical ordering is:
     *
     * ```cpp
     * // any B1 consumers (chord handlers, etc.)
     * locks.Update(phys, now);   // ← ParamLock first
     * pager.Update(phys, now);   // ← Pager last
     * ```
     *
     * @param phys   Physical pot readings, length ≥ NumPots().
     * @param t_ms   Current timestamp (ms); unused today, reserved.
     */
    void PollButtons(uint32_t t_ms, bool gated) override;
    void Update     (const float* phys, uint32_t t_ms) override;

    /**
     * Suppress the next B1-release page advance.
     *
     * Called by any surface that "claims" a B1 gesture in the same frame
     * (e.g. `ParamLock` on a hold-and-nudge, a freeze chord handler on a
     * B1+B2 combination).  See `Update()` for the required call order:
     * consumers must run before the pager.
     */
    void ConsumeButton() override;

    /** The page-advance button, for consume-handshake identity checks. */
    const IButton* PageButton() const override { return b1_; }

    uint8_t Page()        const { return page_; }
    uint8_t NumPages()    const override { return num_pages_; }
    uint8_t NumPots()     const override { return num_pots_; }
    uint8_t ActivePage()  const override { return page_; }

    /** Catch-aware stored value for pot @p p on the current page. */
    float Value(uint8_t pot) const;

    /** Catch-aware stored value for an arbitrary (page, pot). */
    float Stored(uint8_t page, uint8_t pot) const override;

    /**
     * Set a per-page indicator color. ControlLoop reads PageColor(ActivePage())
     * each frame and paints the page-advance button accordingly.
     */
    void          SetPageColor(uint8_t page, LedPanel::Rgb c);
    LedPanel::Rgb PageColor(uint8_t page) const override;

    /** True if the physical pot on the current page has caught the stored value. */
    bool  Caught(uint8_t pot) const;

    /** Raw PotState for an arbitrary (page, pot). */
    PotState State(uint8_t page, uint8_t pot) const override;

    /**
     * Set the stored value for one (page, pot) and re-arm catch — the user
     * must physically move through the new value before it responds.
     */
    void SetStored(uint8_t page, uint8_t pot, float value, const float* phys);

    /** Lock catch on every pot of @p page (e.g. after a preset apply). */
    void LockPage(uint8_t page, const float* phys) override;

    /**
     * Jump straight to @p page, re-arming catch exactly as an advance does.
     *
     * The direct-addressing counterpart to the button's cyclic advance, for
     * callers that must land on a *known* page rather than the next one — a
     * "home" gesture, a preset that pins a page, a host command.  Catch is
     * re-armed through `LockPage()`, so every pot on the destination page
     * stays uncaught until the user moves through its stored value: a jump
     * can never make a parameter leap.
     *
     * Out-of-range @p page is ignored.  Jumping to the page already showing
     * still re-arms catch, so a caller driven by a *held* gesture MUST
     * edge-trigger on the gesture forming — re-issuing this every frame of a
     * hold would keep the pots permanently uncaught.
     *
     * Button-gesture state is deliberately untouched: an advance already
     * latched from a button release this frame still applies on the next
     * `Update()`.  If the gesture that triggered the jump also involves this
     * pager's button, call `ConsumeButton()` alongside it.
     */
    void GoToPage(uint8_t page, const float* phys);

    /* ── Serializable ──────────────────────────────────────────────────
     * Serialises the per-(page, pot) stored values.  Catch state and
     * page index are excluded — both are re-derived against fresh phys
     * on the next Update() (we set a flag in Deserialize that locks
     * every page so the user must re-catch on load).
     */
    size_t   SerializedSize() const override;
    void     Serialize  (uint8_t* out) const override;
    bool     Deserialize(const uint8_t* in)  override;
    uint32_t SchemaHash () const override;

    DescKind DescribeKind() const override { return DescKind::Pager; }

  private:
    IButton*       b1_;
    uint8_t        num_pages_;
    uint8_t        num_pots_;
    uint8_t        page_ = 0;

    /* Button-poll state (updated by PollButtons at 1 ms cadence). */
    bool prev_pressed_     = false;
    bool consume_          = false;
    bool advance_pending_  = false;

    /* Set by Deserialize; cleared by the next Update once we have a
     * phys[] to re-arm against. */
    bool pending_rearm_ = false;

    PotState      states_     [kMaxPages][kMaxPots] = {};
    LedPanel::Rgb page_colors_[kMaxPages]           = {};
};

} // namespace alchemy
