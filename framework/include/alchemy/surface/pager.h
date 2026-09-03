/**
 * @file alchemy/surface/pager.h
 * @brief alchemy::Pager — opt-in pagination + layer surface.
 *
 * Owns the per-(page, pot) pool (pot-catch, presets, VirtualKnob reads)
 * and the navigation that picks the active page, declared per button:
 *
 *   Pager pager = Pager(kNumAppPages, kNumPots)
 *       .Cycle(hw.buttons[kButtonB1], kTone, kSpace) // release advances
 *       .Shift(hw.buttons[kButtonB2], kShiftPage)    // page while held
 *       .Latch(hw.buttons[kButtonB3], kVar, kRnd);   // release toggles
 *
 * A shift layer is a page reached by holding a button: catch, presets,
 * locks, CV, rings and the descriptor come with it, and since catch
 * only runs on the active page a hold cannot smear the pages
 * underneath.  `Pager(b1, n, m)` is unchanged: ≡ `Pager(n, m).Cycle(b1)`.
 * The full contract lives in docs/pages-and-layers.md.
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
    static constexpr uint8_t kMaxPages    = 8u;
    static constexpr uint8_t kMaxPots     = 8u;
    static constexpr uint8_t kMaxBindings = 4u;

    /**
     * Storage-only construction; declare navigation with Cycle() /
     * Shift() / Latch().
     *
     * @param num_pages  Number of pages (1..kMaxPages).  Must be ≥ 1.
     * @param num_pots   Pots per page (1..kMaxPots).
     */
    Pager(uint8_t num_pages, uint8_t num_pots);

    /** Classic form: ≡ `Pager(num_pages, num_pots).Cycle(b1)`. */
    Pager(IButton& b1, uint8_t num_pages, uint8_t num_pots);

    /* ── Navigation declaration.  One binding per button; out-of-range
     *    pages reject a declaration silently (the GoToPage rule). ───── */

    /** Cycle @p b through @p pages on a clean release (empty list =
     *  every page); a Latch pair counts as one slot. */
    template <typename... Pages>
    Pager& Cycle(IButton& b, Pages... pages)
    {
        if (NavBinding* n = AddBinding(NavKind::Cycle, b))
            (AddNavPage(*n, static_cast<uint8_t>(pages)), ...);
        return *this;
    }

    /** Momentary layer: @p page while @p b is held.  Engages on the
     *  press edge, catch re-armed both ways. */
    Pager& Shift(IButton& b, uint8_t page);

    /** Toggle the @p page_a / @p page_b sub-page pair on a clean
     *  release; inert while neither member is showing. */
    Pager& Latch(IButton& b, uint8_t page_a, uint8_t page_b);

    /** Scope the just-declared Shift/Latch to engage only from @p pages. */
    template <typename... Pages>
    Pager& From(Pages... pages)
    {
        if (last_binding_ >= 0 && sizeof...(pages) > 0)
        {
            NavBinding& n = bindings_[static_cast<uint8_t>(last_binding_)];
            if (n.kind != NavKind::Cycle)
            {
                n.from_mask = 0u;
                ((n.from_mask |= PageBit(static_cast<uint8_t>(pages))), ...);
            }
        }
        return *this;
    }

    /** True when the current (or just-released) Shift hold on @p b
     *  edited a parameter — for app-side tap arbitration. */
    bool HoldUsed(const IButton& b) const;

    /* ── View latch (ParamLock, around its trigger hold): the layer
     *    showing at RetainPage() defers its exit until ReleasePage(). ── */

    void RetainPage();
    void ReleasePage();

    /** Edge detection at 1 ms; transitions happen in Update().  When
     *  @p gated, state syncs but nothing fires. */
    void PollButtons(uint32_t t_ms, bool gated) override;

    /** Apply pending navigation, then run pot-catch on the active page.
     *  Surfaces that may ConsumeButton() must run earlier in the frame. */
    void Update(const float* phys, uint32_t t_ms) override;

    /** Suppress this frame's release-driven navigation, and that of any
     *  bound button held right now (a claim with nothing held evaporates
     *  at the end of the frame). */
    void ConsumeButton() override;

    /** The cycle button, for legacy identity checks; nullptr without
     *  one.  Prefer ConsumesRelease(). */
    const IButton* PageButton() const override;

    /** True when a Cycle or Latch binding rides @p b. */
    bool ConsumesRelease(const IButton* b) const override;

    /** True while the Shift hold on @p b has edited a parameter. */
    bool HoldClaimed(const IButton* b) const override;

    /** Per-binding tint: page color on Cycle, layer color while a Shift
     *  is engaged, member color on a Latch's own pair. */
    LedPanel::Rgb IndicatorColor(const IButton* b) const override;

    uint8_t Page()        const { return page_; }
    uint8_t NumPages()    const override { return num_pages_; }
    uint8_t NumPots()     const override { return num_pots_; }
    uint8_t ActivePage()  const override { return page_; }

    /** Catch-aware stored value for pot @p p on the current page. */
    float Value(uint8_t pot) const;

    /** Catch-aware stored value for an arbitrary (page, pot). */
    float Stored(uint8_t page, uint8_t pot) const override;

    /** Per-page indicator color, painted via IndicatorColor(). */
    void          SetPageColor(uint8_t page, LedPanel::Rgb c);
    LedPanel::Rgb PageColor(uint8_t page) const override;

    /** True if the physical pot on the current page has caught the stored value. */
    bool  Caught(uint8_t pot) const;

    /** Raw PotState for an arbitrary (page, pot). */
    PotState State(uint8_t page, uint8_t pot) const override;

    /** Set one (page, pot) stored value and re-arm catch — the user
     *  must move through the new value before it responds. */
    void SetStored(uint8_t page, uint8_t pot, float value, const float* phys);

    /** Lock catch on every pot of @p page (e.g. after a preset apply). */
    void LockPage(uint8_t page, const float* phys) override;

    /**
     * Jump straight to @p page, re-arming catch exactly as an advance
     * does — a jump can never make a parameter leap.  Outranks
     * navigation: an active Shift layer is cancelled outright (no
     * return leg), and a Latch pair containing @p page adopts it.
     *
     * Out-of-range @p page is ignored.  Jumping to the page already
     * showing still re-arms catch, so a caller driven by a *held*
     * gesture MUST edge-trigger on the gesture forming.  Pending
     * button releases are deliberately untouched; if the triggering
     * gesture involves a bound button, call ConsumeButton() alongside.
     */
    void GoToPage(uint8_t page, const float* phys);

    /* ── Serializable ──────────────────────────────────────────────────
     * Serialises the per-(page, pot) stored values.  Catch state, the
     * page index, and navigation state are excluded — re-derived
     * against fresh phys on the next Update() (a layer held through a
     * load stays engaged and re-arms with the rest).
     */
    size_t   SerializedSize() const override;
    void     Serialize  (uint8_t* out) const override;
    bool     Deserialize(const uint8_t* in)  override;
    uint32_t SchemaHash () const override;

    DescKind DescribeKind() const override { return DescKind::Pager; }

  private:
    enum class NavKind : uint8_t { Cycle, Shift, Latch };

    /** One button binding: the cycle list / Shift target / Latch pair
     *  in `pages`, plus poll and gesture state. */
    struct NavBinding
    {
        NavKind  kind             = NavKind::Cycle;
        IButton* btn              = nullptr;
        uint8_t  pages[kMaxPages] = {};
        uint8_t  num_pages        = 0;
        uint8_t  from_mask        = 0xFFu;

        bool prev_pressed    = false;
        bool press_pending   = false;
        bool release_pending = false;
        bool poisoned        = false;   /* press inert until its release   */

        bool    active      = false;    /* Shift layer engaged             */
        bool    used        = false;    /* the hold edited a parameter     */
        bool    lingering   = false;    /* exit deferred by the view latch */
        uint8_t return_page = 0;

        uint8_t latch_sel = 0;          /* which pair member is remembered */
    };

    static uint8_t PageBit(uint8_t page)
    {
        return (page < kMaxPages) ? static_cast<uint8_t>(1u << page) : 0u;
    }

    NavBinding* AddBinding(NavKind kind, IButton& b);
    void        AddNavPage(NavBinding& n, uint8_t page);

    NavBinding*       ActiveOverlay();
    const NavBinding* ActiveOverlay() const;
    const NavBinding* FindBinding(const IButton* b) const;

    void TryEngage      (NavBinding& b, const float* phys);
    void EngageShift    (NavBinding& b, const float* phys);
    void ExitShift      (NavBinding& b, const float* phys);
    void SnapshotOverlay();
    void AbortOverlays  ();
    void DoCycle        (NavBinding& b, const float* phys);
    void DoLatch        (NavBinding& b, const float* phys);
    void Go             (uint8_t page, const float* phys);

    bool    SameSlot    (uint8_t entry, uint8_t cur) const;
    uint8_t ResolveLatch(uint8_t page) const;
    void    SyncLatchSel(uint8_t page);

    uint8_t num_pages_;
    uint8_t num_pots_;
    uint8_t page_ = 0;

    NavBinding bindings_[kMaxBindings] = {};
    uint8_t    num_bindings_           = 0;
    int8_t     last_binding_           = -1;

    bool consume_        = false;
    bool prev_gated_     = false;

    /* A poll-context abort (no phys[]) owes the visible page a catch
     * re-arm, delivered by the next Update. */
    bool relock_pending_ = false;

    /* The one layer the view latch covers (nullptr: none). */
    NavBinding* retained_ov_ = nullptr;

    /* Engage-time snapshot for used-tracking: a pot caught at engage
     * counts as used only once it moves. */
    float   overlay_engage_[kMaxPots] = {};
    uint8_t overlay_caught_mask_      = 0;

    /* Set by Deserialize; cleared by the next Update once we have a
     * phys[] to re-arm against. */
    bool pending_rearm_ = false;

    PotState      states_     [kMaxPages][kMaxPots] = {};
    LedPanel::Rgb page_colors_[kMaxPages]           = {};
};

} // namespace alchemy
