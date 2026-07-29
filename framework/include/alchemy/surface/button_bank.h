/**
 * @file alchemy/surface/button_bank.h
 * @brief alchemy::ButtonBank — storage + gesture backend for VirtualButton.
 *
 * ButtonBank is to buttons what Pager is to knobs: the surface that owns
 * the state.  It persists one byte per stateful VirtualButton, recognizes
 * tap/hold gestures against the active page, paints per-zone button LEDs,
 * and self-describes to HostLink as a "buttons" component whose fields
 * are ordinary editable enums.
 *
 * One deliberate asymmetry with Pager: knob state is keyed by (page, pot)
 * because a physical pot has a position that must be reconciled per page
 * (hence pot-catch); a button has no position, so state is keyed by the
 * VirtualButton *object*.  Pages are pure dispatch scope — the same
 * physical button does different things on different pages by declaring
 * one VirtualButton per page, and moving a button between pages at
 * runtime never touches its state.
 *
 * Usage (with a ControlLoop):
 *
 *   static Page       page_a = Page(0).Knobs(k1, k2).Buttons(b_filter);
 *   static ButtonBank buttons;
 *   ...
 *   presets.Manage(buttons);          // persist + host-edit
 *   loop.Use(pager).Use(page_a).Use(buttons);
 *
 * The roster (which buttons persist, in what byte order) freezes at the
 * first Presets/descriptor walk — the same moment the rest of the blob
 * layout freezes.  Declare every stateful button before BootLoad();
 * page *membership* stays freely mutable afterwards.  Any declaration
 * error (label/color count mismatch, duplicate ident, bad default, a
 * stateful button first seen after freeze) latches Ok() == false and the
 * descriptor build fails — no descriptor beats a wrong one.
 *
 * Gesture contract:
 *   - The bank reads only IButton::Pressed() and derives its own edges,
 *     so app code reading RisingEdge()/FallingEdge() directly keeps
 *     every edge.
 *   - Gestures resolve against the page that was active when the press
 *     began (a mid-hold page change cannot retarget the gesture).
 *   - A gesture fired on the storage's page-advance button consumes the
 *     release (KnobStorage::ConsumeButton), so a button declared on the
 *     pager's B1 shadows page-advance on pages that declare it.
 *   - Do not declare bank gestures on a ParamLock record button — the
 *     two hold gestures cannot be arbitrated.
 *   - Callbacks run in the control-loop poll (same thread as OnPoll);
 *     preset-load changes are applied in Deserialize but notified from
 *     the next Update(), coalescing HostLink live pushes.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "alchemy/surface/serializable.h"
#include "alchemy/surface/virtual_button.h"

namespace alchemy {

class ControlLoop;
class IButton;
class KnobStorage;
class LedPanel;
class Page;

class ButtonBank : public Serializable
{
  public:
    static constexpr uint8_t kMaxCells   = 16u;
    static constexpr uint8_t kMaxModal   = 8u;
    static constexpr uint8_t kMaxPages   = 8u;
    static constexpr uint8_t kMaxGlobals = 4u;
    static constexpr uint8_t kMaxHw      = 8u;

    ButtonBank() = default;

    /* Buttons hold pointers into this bank's cells — copying would
     * leave them aimed at the original. */
    ButtonBank(const ButtonBank&)            = delete;
    ButtonBank& operator=(const ButtonBank&) = delete;

    /* ── Wiring ──────────────────────────────────────────────────────
     * With a ControlLoop, `loop.Use(bank)` does all of this.  The
     * explicit forms serve host tests and loop-less firmware. */

    /** Physical buttons, indexed by VirtualButton::Hw(). */
    ButtonBank& Attach(IButton* const* buttons, uint8_t n)
    {
        hw_btns_ = buttons;
        num_hw_  = (n > kMaxHw) ? kMaxHw : n;
        return *this;
    }

    /** Explicit page roster (overrides any ControlLoop page source). */
    template <typename... Rest>
    ButtonBank& Pages(Page& first, Rest&... rest)
    {
        AddPage(first);
        (AddPage(rest), ...);
        return *this;
    }

    /** A button active on every page. */
    ButtonBank& Global(VirtualButton& b)
    {
        for (uint8_t i = 0; i < num_globals_; i++)
            if (globals_[i] == &b) return *this;
        if (num_globals_ < kMaxGlobals) globals_[num_globals_++] = &b;
        return *this;
    }

    /** Page source fallback when no explicit Pages() were given. */
    void AttachPageSource(const ControlLoop* loop) { loop_ = loop; }

    /** Storage for the consume handshake (re-attached each Tick). */
    void AttachStorage(KnobStorage* storage) { storage_ = storage; }

    /* ── Runtime (ControlLoop cadence, or call directly loop-less) ──── */

    /**
     * Edge detection + gesture dispatch at 1 ms cadence.  When @p gated
     * (Settings active) presses are tracked but nothing fires.
     * @p active_page scopes dispatch and is latched at press time.
     */
    void PollButtons(uint32_t t_ms, bool gated, uint8_t active_page);

    /** Flush deferred change notifications (post-Deserialize).  Called
     *  once per frame, unconditionally — data application is not a
     *  gesture and survives Settings. */
    void Update(uint32_t t_ms);

    /** Paint per-zone colors for the active page's buttons + globals. */
    void Render(LedPanel& panel, uint8_t active_page, uint32_t t_ms) const;

    /* ── State access ────────────────────────────────────────────────
     * VirtualButton::Zone() is the normal read path; these cover
     * programmatic mutation (e.g. cross-page control). */

    uint8_t ZoneOf(const VirtualButton& b) const;

    /** Set a button's zone; out-of-range is ignored.  Notification is
     *  deferred to the next Update() (re-entrancy safe). */
    void SetZone(const VirtualButton& b, uint8_t zone);

    /** False once any declaration error latched (see file header). */
    bool Ok() const { return ok_; }

    /* ── Serializable ────────────────────────────────────────────────── */

    size_t   SerializedSize() const override;
    void     Serialize(uint8_t* out) const override;
    bool     Deserialize(const uint8_t* in) override;
    uint32_t SchemaHash() const override;
    DescKind DescribeKind() const override { return DescKind::Buttons; }

    /* ── Introspection (HostLink descriptor generation) ──────────────
     * All freeze the roster on first use.  Page attribution is the
     * page index when the button lives on exactly one page, else -1
     * (multi-page or global — the descriptor omits the key). */

    uint8_t              NumCells() const;
    const VirtualButton* CellAt(uint8_t i) const;
    int16_t              CellPage(uint8_t i) const;

    uint8_t              NumModal() const;
    const VirtualButton* ModalAt(uint8_t i) const;
    int16_t              ModalPage(uint8_t i) const;

  private:
    struct Cell
    {
        VirtualButton* btn   = nullptr;
        uint8_t        zone  = 0;
        int16_t        page  = -1;
        bool           dirty = false;
    };

    struct ModalRef
    {
        VirtualButton* btn  = nullptr;
        int16_t        page = -1;
    };

    struct HwState
    {
        bool     prev       = false;
        uint32_t press_t    = 0;
        uint8_t  press_page = 0;
        bool     hold_any   = false;   /* a hold fired; suppress the tap */
        uint8_t  hold_fired = 0;       /* per-press bitmask by scan order */
    };

    void AddPage(Page& p)
    {
        for (uint8_t i = 0; i < num_pages_; i++)
            if (pages_[i] == &p) return;
        if (num_pages_ < kMaxPages) pages_[num_pages_++] = &p;
    }

    /* Page source: explicit Pages() wins, else the attached loop. */
    uint8_t     NumSourcePages() const;
    const Page* SourcePage(uint8_t i) const;

    void FreezeIfNeeded() const;
    void EnrollState(VirtualButton& b, int16_t page) const;
    void EnrollModal(VirtualButton& b, int16_t page) const;
    int  FindCell(const VirtualButton& b) const;

    void DispatchPress(uint8_t hw, uint32_t t_ms, uint8_t active_page);
    void DispatchHolds(uint8_t hw, uint32_t t_ms);
    void DispatchTaps(uint8_t hw);
    void FireGesture(VirtualButton& b, const VirtualButton::Gesture& g,
                     uint8_t hw);

    /* Roster — built once, on the first serialization/descriptor walk. */
    mutable bool     frozen_ = false;
    mutable bool     ok_     = true;
    mutable Cell     cells_[kMaxCells];
    mutable uint8_t  num_cells_ = 0;
    mutable ModalRef modal_[kMaxModal];
    mutable uint8_t  num_modal_ = 0;

    /* Hardware. */
    IButton* const* hw_btns_ = nullptr;
    uint8_t         num_hw_  = 0;
    HwState         hw_state_[kMaxHw]    = {};
    bool            pressed_now_[kMaxHw] = {};

    /* Composition. */
    Page*          pages_[kMaxPages]     = {};
    uint8_t        num_pages_            = 0;
    VirtualButton* globals_[kMaxGlobals] = {};
    uint8_t        num_globals_          = 0;

    const ControlLoop* loop_    = nullptr;
    KnobStorage*       storage_ = nullptr;
};

} // namespace alchemy
