/**
 * @file alchemy/surface/page.h
 * @brief alchemy::Page — composable collection of VirtualKnob refs.
 *
 * A Page is a page index plus a list of VirtualKnob pointers. Pages do not
 * own knobs; they only reference them. The same VirtualKnob can appear on
 * multiple pages, no knob at all, or be moved between pages at runtime
 * (e.g. driven by a settings option).
 *
 * ControlLoop reads the live knob list every frame, so changes take effect
 * on the next Tick().
 */

#pragma once

#include <cstdint>
#include "alchemy/surface/virtual_knob.h"

namespace alchemy {

class Page
{
  public:
    static constexpr uint8_t kMaxKnobs = 8u;

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

    /** Empty the page. */
    void Clear() { count_ = 0; }

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
    const char*  TabName () const { return name_; }
    const char*  TabColor() const { return color_; }

  private:
    uint8_t      idx_;
    uint8_t      count_         = 0;
    const char*  name_          = nullptr;
    const char*  color_         = nullptr;
    VirtualKnob* knobs_[kMaxKnobs] = {};
};

} // namespace alchemy
