/**
 * @file alchemy/surface/virtual_button.h
 * @brief alchemy::VirtualButton — declarative button abstraction.
 *
 * VirtualButton is the twin of VirtualKnob: a declarative bundle that
 * names a physical push button, describes what its gestures do, and —
 * when the button carries state — declares that state's shape (zone
 * count, labels, colors, default) exactly once.
 *
 * Like VirtualKnob, the object holds no state of its own.  State lives
 * in a ButtonBank (see alchemy/surface/button_bank.h), the button's
 * storage backend the way Pager is a knob's: the bank persists one byte
 * per stateful button, recognizes tap/hold gestures, paints button LEDs,
 * and self-describes to HostLink as an editable enum field per button.
 * `Zone()` reads through the bank; pages are pure composition
 * (`Page::Buttons(...)`), so the same physical button can do different
 * things on different pages by declaring one VirtualButton per page.
 *
 *   static const char* kModes[3] = {"LP", "BP", "HP"};
 *
 *   static VirtualButton flt = VirtualButton(kButtonB3, "Filter")
 *       .Ident("flt.mode")             // stable host id (default "b<hw>")
 *       .Selector(kModes)              // 3 zones, labels, tap-cycles
 *       .Colors(kModeColors)           // per-zone LED feedback
 *       .Bind(SetFilterMode);          // fires on gesture and preset load
 *
 * Construction mirrors VirtualKnob: physical index + display name, with
 * `.Ident()` pinning the stable field id (defaults to positional
 * "b<hw>"; buttons sharing a hardware index need explicit idents).
 * Host-only state — persisted and browser-editable with no panel
 * gesture — uses the (ident, name) constructor instead.  One edge: a
 * bare literal `0` first argument is ambiguous between the two
 * constructors and fails to compile — pass the board constant
 * (kButtonB1).
 *
 * Defaults are derived: a stateful button with no declared gesture
 * tap-cycles its zones; host gesture labels are derived from the zone
 * labels ("Cycle LP / BP / HP").  Buttons with no `Selector()` are
 * momentary: zero preset bytes, dispatch via `.Tap(fn)` / `.Hold(ms, fn)`,
 * or plain metadata.
 *
 * The pre-ButtonBank metadata form (Role / Action / Controls, emitted by
 * `Host::Buttons()` as the descriptor's root "buttons" array) is
 * unchanged and remains valid, including as constexpr file-scope tables.
 * All strings are held by pointer (string literals or static lifetime).
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "alchemy/led/panel.h"   /* LedPanel::Rgb */

namespace alchemy {

class VirtualButton
{
  public:
    /** Modal buttons drive gestures; State buttons cycle/toggle fields. */
    enum class Role : uint8_t { Modal, State };

    /** Semantic mutation kind for a State button's field controls.
     *  Cycle : each press advances to the next zone (wraps).
     *  Toggle: each press flips 0 ↔ 1 (or between two zones).
     *  Set   : the button drives the field to a specific value on press. */
    enum class Action : uint8_t { Cycle, Toggle, Set };

    /** Capped storage — a real hardware button rarely has more than a
     *  handful of distinct gestures or fields under it, and we want the
     *  table to fit in a bare struct with no allocation. */
    static constexpr uint8_t kMaxActions  = 4;
    static constexpr uint8_t kMaxControls = 4;

    /** Sentinel for an unbound physical index (host-only state). */
    static constexpr uint8_t kNoHw = 0xFFu;

    /** Momentary gesture callback (runs in the control-loop poll). */
    using TapFn = void (*)(void* ctx);

    /** Zone-change notification (gesture, SetZone, or preset load). */
    using ChangeFn = void (*)(uint8_t zone, void* ctx);

    /** One declared gesture slot (at most one tap + one hold). */
    struct Gesture
    {
        bool        used     = false;
        enum Action action   = Action::Cycle;
        uint8_t     set_zone = 0;
        uint16_t    hold_ms  = 0;
        const char* label    = nullptr;
        TapFn       fn       = nullptr;
        void*       fn_ctx   = nullptr;
    };

    /**
     * Physical button, mirroring VirtualKnob(pot, name).
     *
     * @param hw    Physical button index (e.g. kButtonB3).  A bare
     *              literal 0 is ambiguous with the ctor below — pass
     *              the board constant.
     * @param name  Display name (e.g. "Filter").
     */
    constexpr VirtualButton(uint8_t hw, const char* name)
        : ident_(nullptr), name_(name), hw_(hw) {}

    /**
     * Host-only state (no panel gesture) and legacy metadata tables.
     *
     * @param ident  Stable id (e.g. "flt.mode"). Hosts key on this — never
     *               rename (a rename also invalidates saved presets once
     *               the button is bank-managed).
     * @param name   Display name.
     */
    constexpr VirtualButton(const char* ident, const char* name)
        : ident_(ident), name_(name) {}

    /* ── Configuration (builder, string literals only) ───────────────── */

    constexpr VirtualButton& Role(enum Role r) { role_ = r; return *this; }

    /** Declare a labeled gesture the button performs.  Silently drops
     *  entries past kMaxActions — a hardware button with more than that
     *  is a design smell, not a runtime error. */
    constexpr VirtualButton& Action(const char* gesture, const char* label)
    {
        if (num_actions_ < kMaxActions)
        {
            actions_[num_actions_].gesture = gesture;
            actions_[num_actions_].label   = label;
            ++num_actions_;
        }
        return *this;
    }

    /** For State-role buttons: declare that this button mutates the
     *  named field with the given action.  Field ids are the same ids
     *  the field's owning component emits (looked up via fieldById). */
    constexpr VirtualButton& Controls(const char* field_id, enum Action action)
    {
        if (num_controls_ < kMaxControls)
        {
            controls_[num_controls_].field  = field_id;
            controls_[num_controls_].action = action;
            ++num_controls_;
        }
        return *this;
    }

    /* ── Identity + state shape (ButtonBank-managed buttons) ─────────── */

    /** Stable field id (e.g. "flt.mode").  Defaults to the positional
     *  "b<hw>"; set one to keep host presets addressable when the
     *  button later moves, and always when two buttons share a
     *  hardware index. */
    constexpr VirtualButton& Ident(const char* id)
    {
        ident_ = id;
        return *this;
    }

    /** N-zone persisted state (N ≥ 2), one byte in the preset blob. */
    constexpr VirtualButton& Selector(uint8_t zones)
    {
        zones_ = zones;
        return *this;
    }

    /** Labeled form: zone count and labels deduced from one array. */
    template <size_t N>
    constexpr VirtualButton& Selector(const char* const (&labels)[N])
    {
        zones_      = static_cast<uint8_t>(N);
        labels_     = labels;
        num_labels_ = static_cast<uint8_t>(N);
        return *this;
    }

    /** Two-zone state (on/off). */
    constexpr VirtualButton& Toggle() { return Selector(2); }

    /** Factory-default zone (must be < zones; checked at bank freeze). */
    constexpr VirtualButton& Default(uint8_t zone)
    {
        default_zone_ = zone;
        return *this;
    }

    /** Zone labels, count checked against zones at bank freeze. */
    constexpr VirtualButton& Labels(const char* const* labels, uint8_t n)
    {
        labels_     = labels;
        num_labels_ = n;
        return *this;
    }

    template <size_t N>
    constexpr VirtualButton& Labels(const char* const (&labels)[N])
    {
        return Labels(labels, static_cast<uint8_t>(N));
    }

    /** Per-zone LED colors painted by the bank while this button's page
     *  is active.  Count checked against zones at bank freeze (pass
     *  n == 0 to skip the check for a non-array source). */
    constexpr VirtualButton& Colors(const LedPanel::Rgb* per_zone, uint8_t n = 0)
    {
        colors_     = per_zone;
        num_colors_ = n;
        return *this;
    }

    template <size_t N>
    constexpr VirtualButton& Colors(const LedPanel::Rgb (&per_zone)[N])
    {
        return Colors(per_zone, static_cast<uint8_t>(N));
    }

    /* ── Gestures (at most one tap + one hold) ───────────────────────────
     * A stateful button with no declared gesture tap-cycles by default
     * (emitted as "toggle" when zones == 2).  Labels are optional — the
     * descriptor derives them from the zone labels. */

    constexpr VirtualButton& Tap(enum Action a, const char* label = nullptr)
    {
        tap_.used   = true;
        tap_.action = a;
        tap_.label  = label;
        return *this;
    }

    /** Momentary tap: calls @p fn instead of mutating state. */
    constexpr VirtualButton& Tap(TapFn fn, const char* label = nullptr,
                                 void* ctx = nullptr)
    {
        tap_.used   = true;
        tap_.fn     = fn;
        tap_.fn_ctx = ctx;
        tap_.label  = label;
        return *this;
    }

    /** Tap sets the state to a specific zone. */
    constexpr VirtualButton& TapSet(uint8_t zone, const char* label = nullptr)
    {
        tap_.used     = true;
        tap_.action   = Action::Set;
        tap_.set_zone = zone;
        tap_.label    = label;
        return *this;
    }

    constexpr VirtualButton& Hold(uint32_t ms, enum Action a,
                                  const char* label = nullptr)
    {
        hold_.used    = true;
        hold_.action  = a;
        hold_.hold_ms = ClampHoldMs(ms);
        hold_.label   = label;
        return *this;
    }

    constexpr VirtualButton& Hold(uint32_t ms, TapFn fn,
                                  const char* label = nullptr,
                                  void* ctx = nullptr)
    {
        hold_.used    = true;
        hold_.fn      = fn;
        hold_.fn_ctx  = ctx;
        hold_.hold_ms = ClampHoldMs(ms);
        hold_.label   = label;
        return *this;
    }

    constexpr VirtualButton& HoldSet(uint32_t ms, uint8_t zone,
                                     const char* label = nullptr)
    {
        hold_.used     = true;
        hold_.action   = Action::Set;
        hold_.set_zone = zone;
        hold_.hold_ms  = ClampHoldMs(ms);
        hold_.label    = label;
        return *this;
    }

    /* ── Effect ──────────────────────────────────────────────────────── */

    /** Write the zone into @p target on every change (gesture or load). */
    template <typename T>
    constexpr VirtualButton& Bind(T* target)
    {
        bind_obj_fn_  = [](uint8_t zone, void* ctx)
        { *static_cast<T*>(ctx) = static_cast<T>(zone); };
        bind_obj_ctx_ = target;
        return *this;
    }

    /** Call @p setter with the zone (cast to its enum/integral parameter
     *  type) on every change. */
    template <typename T>
    VirtualButton& Bind(void (*setter)(T))
    {
        bind_raw_  = reinterpret_cast<void (*)()>(setter);
        bind_call_ = [](void (*raw)(), uint8_t zone)
        { reinterpret_cast<void (*)(T)>(raw)(static_cast<T>(zone)); };
        return *this;
    }

    /** General change notification (fires after any Bind targets). */
    constexpr VirtualButton& OnChange(ChangeFn fn, void* ctx = nullptr)
    {
        change_fn_  = fn;
        change_ctx_ = ctx;
        return *this;
    }

    /* ── Host descriptor metadata (optional) ─────────────────────────── */

    /** Attach this button to the named field (e.g. its companion knob
     *  "flt.cutoff"): hosts render it with that field wherever the
     *  field appears, falling back to the page's plain button group
     *  where it doesn't.  The id must name a field the descriptor
     *  emits — a miss fails the descriptor build. */
    constexpr VirtualButton& Anchor(const char* field_id)
    {
        anchor_ = field_id;
        return *this;
    }

    /** Raw display-hint JSON; overrides the derived enum hint. */
    constexpr VirtualButton& Disp(const char* disp_json)
    {
        disp_ = disp_json;
        return *this;
    }

    /* ── Reads (ISR-safe; state lives in the ButtonBank) ─────────────── */

    /** Current zone (Default() until a bank adopts this button). */
    uint8_t Zone() const { return zone_ptr_ ? *zone_ptr_ : default_zone_; }

    /** Zone as float, matching a Selector knob's Value() semantics. */
    float Value() const { return static_cast<float>(Zone()); }

    /** Live debounced state of the bound physical button. */
    bool Pressed() const { return pressed_ptr_ && *pressed_ptr_; }

    /* ── Accessors (descriptor emitters + ButtonBank) ────────────────── */

    constexpr const char* Ident() const { return ident_; }
    constexpr const char* Name()  const { return name_; }
    constexpr enum Role   RoleValue() const { return role_; }

    constexpr uint8_t     NumActions() const { return num_actions_; }
    constexpr const char* ActionGesture(uint8_t i) const { return actions_[i].gesture; }
    constexpr const char* ActionLabel  (uint8_t i) const { return actions_[i].label;   }

    constexpr uint8_t     NumControls() const { return num_controls_; }
    constexpr const char* ControlField (uint8_t i) const { return controls_[i].field;  }
    constexpr enum Action ControlAction(uint8_t i) const { return controls_[i].action; }

    constexpr uint8_t  HwIndex()     const { return hw_; }
    constexpr bool     HasHw()       const { return hw_ != kNoHw; }
    constexpr bool     HasState()    const { return zones_ > 0u; }
    constexpr uint8_t  Zones()       const { return zones_; }
    constexpr uint8_t  DefaultZone() const { return default_zone_; }

    constexpr const char* const* ZoneLabels()    const { return labels_; }
    constexpr uint8_t            NumZoneLabels() const { return num_labels_; }

    constexpr const LedPanel::Rgb* ZoneColors()    const { return colors_; }
    constexpr uint8_t              NumZoneColors() const { return num_colors_; }

    constexpr const Gesture& TapGesture()  const { return tap_; }
    constexpr const Gesture& HoldGesture() const { return hold_; }

    constexpr const char* AnchorField() const { return anchor_; }
    constexpr const char* DispJson()  const { return disp_; }

    /** Fire the change targets (Bind then OnChange).  ButtonBank calls
     *  this on gesture mutation and deferred preset application. */
    void NotifyChange(uint8_t zone) const
    {
        if (bind_obj_fn_) bind_obj_fn_(zone, bind_obj_ctx_);
        if (bind_call_)   bind_call_(bind_raw_, zone);
        if (change_fn_)   change_fn_(zone, change_ctx_);
    }

    /* ── Wiring (called by ButtonBank at roster freeze) ──────────────── */

    void Attach(const uint8_t* zone_ptr, const bool* pressed_ptr)
    {
        zone_ptr_    = zone_ptr;
        pressed_ptr_ = pressed_ptr;
    }

    /** Wire-name for a role.  Kept as a string on the wire so hosts can
     *  forward-tolerate roles this SDK doesn't know yet. */
    static constexpr const char* RoleName(enum Role r)
    {
        switch (r)
        {
            case Role::State: return "state";
            case Role::Modal: /* fall through */
            default:          return "modal";
        }
    }

    /** Wire-name for an action.  Same forward-tolerance rule. */
    static constexpr const char* ActionName(enum Action a)
    {
        switch (a)
        {
            case Action::Toggle: return "toggle";
            case Action::Set:    return "set";
            case Action::Cycle:  /* fall through */
            default:             return "cycle";
        }
    }

  private:
    static constexpr uint16_t ClampHoldMs(uint32_t ms)
    {
        return (ms > 0xFFFFu) ? 0xFFFFu : static_cast<uint16_t>(ms);
    }

    struct ActionEntry  { const char* gesture = nullptr; const char* label = nullptr; };
    struct ControlEntry { const char* field   = nullptr; enum Action action = Action::Cycle; };

    const char*  ident_;
    const char*  name_;
    enum Role    role_          = Role::Modal;
    ActionEntry  actions_[kMaxActions]   = {};
    uint8_t      num_actions_   = 0;
    ControlEntry controls_[kMaxControls] = {};
    uint8_t      num_controls_  = 0;

    /* Hardware + state shape. */
    uint8_t hw_           = kNoHw;
    uint8_t zones_        = 0;
    uint8_t default_zone_ = 0;

    const char* const*   labels_     = nullptr;
    uint8_t              num_labels_ = 0;
    const LedPanel::Rgb* colors_     = nullptr;
    uint8_t              num_colors_ = 0;

    /* Gestures. */
    Gesture tap_  = {};
    Gesture hold_ = {};

    /* Change targets. */
    ChangeFn bind_obj_fn_             = nullptr;
    void*    bind_obj_ctx_            = nullptr;
    void  (*bind_raw_)()              = nullptr;
    void  (*bind_call_)(void (*)(), uint8_t) = nullptr;
    ChangeFn change_fn_               = nullptr;
    void*    change_ctx_              = nullptr;

    /* Host metadata. */
    const char* anchor_ = nullptr;
    const char* disp_ = nullptr;

    /* Wiring (set by ButtonBank). */
    const uint8_t* zone_ptr_    = nullptr;
    const bool*    pressed_ptr_ = nullptr;
};

} // namespace alchemy
