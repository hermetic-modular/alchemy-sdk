/**
 * @file alchemy/surface/manual.h
 * @brief alchemy::Manual — module-level manual content (tagline, preamble,
 *        sections, preset-help override), plus the thin stock help texts.
 *
 * Entity-scoped prose attaches to the entity itself (.Help()); Manual
 * carries only what has no object.  Stock defaults exist ONLY for the
 * SDK-owned features (presets, locks, SD, brightness) — nothing else ever
 * gets default prose.  One layer of CommonMark+GFM; never restate
 * structured facts (ranges, defaults, slot counts).
 */

#pragma once

#include <cstdint>

namespace alchemy {

/* ── Stock help — the SDK-owned feature texts, override via .Help() ──── */

namespace stock_help {

/* Count-free on purpose — counts are structured facts hosts render. */

inline constexpr const char* kPresets =
    "Preset slots store the complete state of the module — every page, "
    "every setting marked as preset material, and everything else the "
    "firmware manages. Slot 0 is the boot slot: it loads automatically at "
    "power-up. Presets are saved and named from the web programmer, or "
    "from the panel using the module's preset gestures. Slots written by "
    "a different firmware version are flagged in the programmer and can "
    "be migrated with **Back up all**.";

inline constexpr const char* kLocks =
    "Parameter locks are short automation loops recorded from the panel: "
    "hold the lock gesture and move a knob to capture the motion, and the "
    "module replays it as part of the patch.";

inline constexpr const char* kStorage =
    "The SD card holds files the firmware reads and writes. Browse and "
    "manage them from the web programmer's file panel. Avoid removing the "
    "card while the module is writing; everything else is safe at any "
    "time.";

inline constexpr const char* kBrightness =
    "Global LED brightness control in safe ranges. Uses more power.";

} // namespace stock_help

/* ── Manual root ─────────────────────────────────────────────────────── */

class Manual
{
  public:
    static constexpr uint8_t kMaxSections = 8u;

    struct SectionDecl
    {
        const char* id    = nullptr; /* slug, becomes the deep-link id */
        const char* title = nullptr;
        const char* body  = nullptr; /* markdown */
    };

    constexpr Manual() = default;

    /** One-line identity; rides the descriptor's module{} block. */
    constexpr Manual& Tagline(const char* t) { tagline_ = t; return *this; }

    /** Opening prose at the top of the manual page (markdown). */
    constexpr Manual& Preamble(const char* md) { preamble_ = md; return *this; }

    /** Long-form section; overflow fails the descriptor build. */
    constexpr Manual& Section(const char* id, const char* title,
                              const char* body_md)
    {
        if (num_sections_ < kMaxSections)
            sections_[num_sections_++] = {id, title, body_md};
        else
            overflow_ = true;
        return *this;
    }

    /** Override the stock preset-system help (stock_help::kPresets). */
    constexpr Manual& PresetsHelp(const char* md)
    {
        presets_help_ = md;
        return *this;
    }

    constexpr const char* TaglineText()  const { return tagline_; }
    constexpr const char* PreambleText() const { return preamble_; }
    constexpr uint8_t NumSections() const { return num_sections_; }
    constexpr const SectionDecl& SectionAt(uint8_t i) const { return sections_[i]; }
    constexpr const char* PresetsHelpText() const { return presets_help_; }
    constexpr bool Overflowed() const { return overflow_; }

  private:
    const char* tagline_      = nullptr;
    const char* preamble_     = nullptr;
    const char* presets_help_ = nullptr;
    SectionDecl sections_[kMaxSections] = {};
    uint8_t     num_sections_ = 0u;
    bool        overflow_     = false;
};

} // namespace alchemy
