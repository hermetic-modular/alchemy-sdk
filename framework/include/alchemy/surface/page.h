/**
 * @file alchemy/surface/page.h
 * @brief alchemy::Page — composable collection of VirtualKnob and
 *        VirtualButton refs.
 *
 * A Page is a page index plus lists of VirtualKnob and VirtualButton
 * pointers. Pages own neither; they only reference them. The same knob or
 * button can appear on multiple pages, no page at all, or be moved between
 * pages at runtime (e.g. driven by a settings option).
 *
 * ControlLoop (and ButtonBank) read the live lists every frame, so changes
 * take effect on the next Tick().
 */

#pragma once

#include <cstdint>
#include "alchemy/surface/virtual_knob.h"

namespace alchemy {

class VirtualButton;

class Page
{
  public:
    static constexpr uint8_t kMaxKnobs   = 8u;
    static constexpr uint8_t kMaxButtons = 4u;

    /** Construct a page bound to @p page_idx on whatever KnobStorage is attached. */
    explicit Page(uint8_t page_idx) : idx_(page_idx) {}

    /* ── Composition ──────────────────────────────────────────────────── */

    /**
     * Append a list of VirtualKnobs. Fluent; returns *this so the canonical
     * declaration is `Page(0).Knobs(k1, k2, ...)`. Pages stay below their
     * kMaxKnobs limit; overflow is silently dropped (a single page should
     * never need more than the board's pot count).
     */
    template <typename... Rest>
    Page& Knobs(VirtualKnob& first, Rest&... rest)
    {
        Add(first);
        (Add(rest), ...);
        return *this;
    }

    /** Append one knob. Idempotent: a duplicate add is a no-op. */
    void Add(VirtualKnob& k)
    {
        for (uint8_t i = 0; i < count_; i++)
            if (knobs_[i] == &k) return;
        if (count_ < kMaxKnobs) knobs_[count_++] = &k;
    }

    /** Remove one knob if present. Shifts subsequent entries down. */
    void Remove(VirtualKnob& k)
    {
        for (uint8_t i = 0; i < count_; i++)
        {
            if (knobs_[i] == &k)
            {
                for (uint8_t j = i; j + 1u < count_; j++)
                    knobs_[j] = knobs_[j + 1];
                count_--;
                return;
            }
        }
    }

    /** Empty the page's knob list. */
    void Clear() { count_ = 0; }

    /**
     * Append a list of VirtualButtons — the buttons active while this
     * page is visible.  A ButtonBank attached to the same loop persists,
     * dispatches, and describes them per page.  Overflow past kMaxButtons
     * is silently dropped, mirroring Knobs().
     */
    template <typename... Rest>
    Page& Buttons(VirtualButton& first, Rest&... rest)
    {
        AddButton(first);
        (AddButton(rest), ...);
        return *this;
    }

    /** Append one button. Idempotent: a duplicate add is a no-op. */
    void AddButton(VirtualButton& b)
    {
        for (uint8_t i = 0; i < num_buttons_; i++)
            if (buttons_[i] == &b) return;
        if (num_buttons_ < kMaxButtons) buttons_[num_buttons_++] = &b;
    }

    /** Remove one button if present. Shifts subsequent entries down. */
    void RemoveButton(VirtualButton& b)
    {
        for (uint8_t i = 0; i < num_buttons_; i++)
        {
            if (buttons_[i] == &b)
            {
                for (uint8_t j = i; j + 1u < num_buttons_; j++)
                    buttons_[j] = buttons_[j + 1];
                num_buttons_--;
                return;
            }
        }
    }

    /* ── Tab identity (optional; HostLink descriptors label and tint the
     *    web editor's page tabs from these — pass string literals) ────── */

    Page& Name (const char* name)    { name_  = name;  return *this; }
    Page& Color(const char* css_hex) { color_ = css_hex; return *this; }

    /* ── Read accessors ───────────────────────────────────────────────── */

    uint8_t      Index() const { return idx_; }
    uint8_t      Count() const { return count_; }
    VirtualKnob* At(uint8_t i) const
    {
        return (i < count_) ? knobs_[i] : nullptr;
    }
    uint8_t        NumButtons() const { return num_buttons_; }
    VirtualButton* ButtonAt(uint8_t i) const
    {
        return (i < num_buttons_) ? buttons_[i] : nullptr;
    }
    const char*  TabName () const { return name_; }
    const char*  TabColor() const { return color_; }

  private:
    uint8_t      idx_;
    uint8_t      count_         = 0;
    const char*  name_          = nullptr;
    const char*  color_         = nullptr;
    VirtualKnob* knobs_[kMaxKnobs] = {};

    VirtualButton* buttons_[kMaxButtons] = {};
    uint8_t        num_buttons_          = 0;
};

} // namespace alchemy
