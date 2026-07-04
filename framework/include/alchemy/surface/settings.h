/**
 * @file alchemy/surface/settings.h
 * @brief alchemy::Settings — opt-in settings-mode surface.
 *
 * Settings is two things in one type:
 *
 *   1. A gesture — B2+B3 held for hold_ms enters; B2 or B3 Exits.
 *
 *   2. A declarative screen — when constructed with `AlchemyLab&`,
 *      Settings owns its own page-cycling (B1 short-press while active),
 *      its own per-(page, pot) catch state, render, and a Serializable
 *      view of every persistent control.  Standard options (brightness,
 *      preset gestures) are one method call each; custom controls go
 *      through the same `Page(i).Pot(p).*` builder.
 *
 */

#pragma once

#include <cstdint>
#include "alchemy/control/pot_catch.h"
#include "alchemy/hw/alchemy_lab_layout.h"   /* kNumPots */
#include "alchemy/hw/i_button.h"
#include "alchemy/led/panel.h"
#include "alchemy/surface/preset_gesture_ui.h"
#include "alchemy/surface/serializable.h"
#include "alchemy/surface/settings_control.h"

namespace alchemy {

#if defined(ALCHEMY_BOARD_V2)
class AlchemyLabV2;
using AlchemyLab = AlchemyLabV2;
#else
class AlchemyLabV1;
using AlchemyLab = AlchemyLabV1;
#endif
class Pager;
class Presets;

/** Default hold duration (ms) for the B2+B3 settings-enter gesture. */
constexpr uint32_t kDefaultSettingsHoldMs = 2000u;

/** Maximum settings pages. */
constexpr uint8_t  kSettingsMaxPages      = 4u;

/** Default color used by Render() to paint B1 while settings is active. */
constexpr LedPanel::Rgb kSettingsDefaultIndicator = {0xFF, 0x50, 0x00};

class Settings : public Serializable
{
  public:
    /* ── Legacy flag-only ctor (unchanged behavior) ─────────────── */
    Settings(IButton& b2, IButton& b3,
             uint32_t hold_ms = kDefaultSettingsHoldMs);

    /* ── Rich-mode ctor ──────────────────────────────────────────── */
    /**
     * @param hw          Board.  Settings claims B1 only while active,
     *                    B2/B3 always for the enter/exit gesture.
     * @param perf_pager  Optional perf-mode Pager.  When non-null,
     *                    Settings calls perf_pager->LockPage() on exit
     *                    so pots re-catch in perf mode automatically.
     * @param hold_ms     B2+B3 hold duration for entry.
     */
    Settings(AlchemyLab&   hw,
             Pager*        perf_pager = nullptr,
             uint32_t      hold_ms    = kDefaultSettingsHoldMs);

    /* ── Standard options ────────────────────────────────────────── */

    /**
     * Brightness control.  Defaults to (page=0, pot=0), range [0.05..1.00],
     * default 0.30.  Override placement with `.At(page, pot)` on the
     * returned handle, or override range with `.Range(lo, hi).Default(v)`.
     * BE CAREFUL! These LEDs get insanely bright. And hot. Don't push it!
     */
    BrightnessHandle UseBrightness();

    /**
     * Preset save/load gesture.  Reserves (page=0, pot=2) for the slot
     * selector and (page=0, pot=3) for the action.  Owns an internal
     * PresetGestureUi bound to @p store; the gesture commits invoke
     * @p store .Save() / .Load() directly.
     */
    PresetGestureUi& UsePresets(Presets& store);

    /* ── Declarative custom controls ─────────────────────────────── */

    class PotBuilder
    {
      public:
        PotBuilder(Settings& s, uint8_t page, uint8_t pot)
            : s_(s), page_(page), pot_(pot) {}

        BrightnessHandle Brightness();
        KnobHandle       Knob   ();
        BipolarHandle    Bipolar();
        SelectorHandle   Selector(uint8_t num_zones);
        CustomHandle     Custom ();

      private:
        Settings& s_;
        uint8_t   page_;
        uint8_t   pot_;
    };

    class PageBuilder
    {
      public:
        PageBuilder(Settings& s, uint8_t page) : s_(s), page_(page) {}
        PotBuilder Pot(uint8_t p) { return PotBuilder(s_, page_, p); }

        /** Tab label for this page in HostLink hosts (string literal). */
        PageBuilder& Name(const char* name)
        {
            s_.page_names_[page_] = name;
            return *this;
        }

      private:
        Settings& s_;
        uint8_t   page_;
    };

    /** Access a settings page (auto-creates pages 1..N-1 on first reference). */
    PageBuilder Page(uint8_t idx = 0);

    uint8_t NumPages() const { return num_pages_; }

    /** Index of the currently visible settings page (0..NumPages()-1).
     *  Always returns 0 in the legacy flag-only ctor mode (no pages). */
    uint8_t CurrentPage() const { return page_; }

    /* ── Lifecycle ───────────────────────────────────────────────── */

    /**
     * Detect mode-gesture (B2+B3 hold) and B1 page-cycle edges at 1 ms
     * cadence. Called from ControlLoop's inner poll loop. Sets internal
     * flags consumed by Update().
     */
    void PollButtons(uint32_t t_ms);

    /** Legacy flag-only Update — equivalent to Update(nullptr, t_ms). */
    void Update(uint32_t t_ms);

    /** Rich Update — required for any declarative control to function. */
    void Update(const float* phys, uint32_t t_ms);

    /**
     * Render every control on the active settings page + the button
     * indicator.  No-op in flag-only mode.  Caller still owns
     * hw.leds.Clear() / .Show() bracketing.
     */
    void Render(uint32_t t_ms) const;

    bool IsActive() const { return active_; }

    /** Color painted on B1's button-pair while IsActive(). */
    void SetIndicatorColor(LedPanel::Rgb c) { indicator_color_ = c; }

    /* ── Serializable ────────────────────────────────────────────── */
    size_t   SerializedSize() const override;
    void     Serialize  (uint8_t* out) const override;
    bool     Deserialize(const uint8_t* in)  override;
    uint32_t SchemaHash () const override;

    /** ── Introspection (HostLink descriptor generation) ──── */

    /** Control kind at (page, pot); None when out of range. */
    SettingsKind KindAt(uint8_t page, uint8_t pot) const;

    /** Selector zone count at (page, pot); 0 for non-selectors. */
    uint8_t ZonesAt(uint8_t page, uint8_t pot) const;

    /** Pot-normalized stored value — the float Serialize() emits. */
    float StoredNormAt(uint8_t page, uint8_t pot) const;

    /** Selector index — the byte Serialize() emits. */
    uint8_t SelectorIdxAt(uint8_t page, uint8_t pot) const;

    /** Logical range for Brightness controls (lo/hi). */
    float RangeLoAt(uint8_t page, uint8_t pot) const;
    float RangeHiAt(uint8_t page, uint8_t pot) const;

    /** Bytes Serialize() emits for a control of kind @p k. */
    static size_t PersistedBytesFor(SettingsKind k);

    /** Tab label declared via Page(pg).Name(...); nullptr when unset. */
    const char* PageNameAt(uint8_t page) const
    {
        return (page < kSettingsMaxPages) ? page_names_[page] : nullptr;
    }

    DescKind DescribeKind() const override { return DescKind::Settings; }

  private:
    friend class PotBuilder;

    /* HostLink tab labels (set via PageBuilder::Name). */
    const char* page_names_[kSettingsMaxPages] = {};

    /* Mode-flag gesture state. */
    IButton*       b2_              = nullptr;
    IButton*       b3_              = nullptr;
    uint32_t       hold_ms_         = kDefaultSettingsHoldMs;
    bool           active_          = false;
    bool           both_held_active_= false;
    uint32_t       both_held_since_ = 0u;
    bool           prev_b2_pressed_ = false;
    bool           prev_b3_pressed_ = false;

    /* Rich-mode wiring (null when constructed via the legacy ctor). */
    AlchemyLab*    hw_              = nullptr;
    Pager*         perf_pager_      = nullptr;
    IButton*       b1_              = nullptr;
    bool           prev_b1_pressed_ = false;
    uint32_t       last_t_ms_       = 0u;

    /* Rich-mode page model. */
    uint8_t      page_      = 0;
    uint8_t      num_pages_ = 1;
    SettingsSlot slots_[kSettingsMaxPages][kNumPots] = {};

    /* Preset binding (rich mode only). */
    PresetGestureUi preset_gesture_;
    bool            uses_presets_     = false;
    uint8_t         preset_slot_pot_  = 2;
    uint8_t         preset_action_pot_= 3;

    LedPanel::Rgb indicator_color_ = kSettingsDefaultIndicator;

    /* Lazy re-arm after a Deserialize — we need a phys[] to lock against. */
    bool pending_rearm_ = false;

    /* Edge-detection flags set by PollButtons, consumed by Update. */
    bool settings_advance_pending_ = false;
    bool prev_update_active_       = false;
    bool gesture_polled_           = false;

    /* Helpers. */
    void EnsurePage(uint8_t page);
    void TickModeGesture(uint32_t t_ms);
    void ArmPage(uint8_t page, const float* phys);
    void ApplyBrightnessSlot(SettingsSlot& s);
};

} // namespace alchemy
