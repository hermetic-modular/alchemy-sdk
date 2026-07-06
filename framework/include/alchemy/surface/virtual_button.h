/**
 * @file alchemy/surface/virtual_button.h
 * @brief alchemy::VirtualButton — declarative button metadata.
 *
 * VirtualButton is the twin of VirtualKnob: a small, declarative bundle
 * that names a physical push button, describes what its gestures do, and
 * — when the button drives persistent state — points at the preset
 * field(s) it mutates.
 *
 * The button itself is a hardware IButton (or a Pager / ParamLock owner
 * of one).  VirtualButton adds *nothing* to the runtime path — it is
 * pure metadata, consumed by the HostLink descriptor so the web tool can
 * render meaningful button chips, gesture hints, and (for state buttons)
 * a "controls this field" back-reference.
 *
 * Usage:
 *
 *   using B = alchemy::VirtualButton;
 *   constexpr B b_source = B("b2", "Osc Source")
 *       .Role(B::Role::State)
 *       .Action("tap", "Cycle Int / Ext / Both")
 *       .Controls("modes.source", B::Action::Cycle);
 *
 *   constexpr B b_pager = B("b1", "Page / Lock")
 *       .Role(B::Role::Modal)
 *       .Action("tap",       "Next Page")
 *       .Action("hold+knob", "Record Param Lock");
 *
 * "Modal" buttons drive transient gestures (page advance, lock record) —
 * they carry no persistent state and their only preset relationship is
 * *incidental* (e.g. the pot values a locked knob captures).  "State"
 * buttons cycle or toggle one or more preset fields; the descriptor
 * points at those fields by id so the host can show the current value
 * next to the button and read it back consistently.
 *
 * All strings are held by pointer (string literals or static lifetime).
 * No allocation, no destructors — safe as a constexpr file-scope table.
 */

#pragma once

#include <cstddef>
#include <cstdint>

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

    /**
     * @param ident  Stable id (e.g. "b1"). Hosts key on this — never rename.
     * @param name   Display name (e.g. "Page / Lock").
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

    /* ── Accessors (used by the descriptor emitter) ──────────────────── */

    constexpr const char* Ident() const { return ident_; }
    constexpr const char* Name()  const { return name_; }
    constexpr enum Role   RoleValue() const { return role_; }

    constexpr uint8_t     NumActions() const { return num_actions_; }
    constexpr const char* ActionGesture(uint8_t i) const { return actions_[i].gesture; }
    constexpr const char* ActionLabel  (uint8_t i) const { return actions_[i].label;   }

    constexpr uint8_t     NumControls() const { return num_controls_; }
    constexpr const char* ControlField (uint8_t i) const { return controls_[i].field;  }
    constexpr enum Action ControlAction(uint8_t i) const { return controls_[i].action; }

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
    struct ActionEntry  { const char* gesture = nullptr; const char* label = nullptr; };
    struct ControlEntry { const char* field   = nullptr; enum Action action = Action::Cycle; };

    const char*  ident_;
    const char*  name_;
    enum Role    role_          = Role::Modal;
    ActionEntry  actions_[kMaxActions]   = {};
    uint8_t      num_actions_   = 0;
    ControlEntry controls_[kMaxControls] = {};
    uint8_t      num_controls_  = 0;
};

} // namespace alchemy
