/**
 * @file alchemy/surface/settings_control.h
 * @brief Per-pot data slot + typed handles used by Settings's declarative
 *        page builder.
 *
 * One SettingsSlot lives at each (page, pot) coordinate inside a Settings
 * instance.  Its `kind` field selects which fields are meaningful — the
 * struct is intentionally a flat POD rather than a tagged union so the
 * dispatch tables in settings.cpp stay readable and no placement-new is
 * required.
 *
 * The chained builders below (BrightnessHandle, SelectorHandle, etc.) are
 * thin pointer-wrappers that the page builder returns to the user.  They
 * let the user keep configuring the slot after the kind is chosen and
 * later read the resolved value:
 *
 *   auto cv_dest = settings.Page(0).Pot(1)
 *                      .Selector(3).Default(2).Colors(...);
 *   ...
 *   const uint8_t idx = cv_dest.Value();
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include "alchemy/control/pot_catch.h"
#include "alchemy/led/anims/fill.h"
#include "alchemy/led/anims/selector.h"
#include "alchemy/led/panel.h"
#include "alchemy/surface/see_ref.h"

namespace alchemy {

class LockSource;
class PresetGestureUi;
class Settings;
struct SettingsSlot;

/* ── Per-slot kind enum ────────────────────────────────────────────── */

enum class SettingsKind : uint8_t
{
    None = 0,
    Brightness,
    Knob,
    Bipolar,
    Selector,
    PresetSlotPot,
    PresetActionPot,
    Custom,
};

/* ── Custom-control hook function-pointer types ─────────────────────── */

using SettingsTickFn   = void (*)(SettingsSlot& slot, float phys, uint32_t t_ms);
using SettingsRenderFn = void (*)(const SettingsSlot& slot,
                                  LedPanel& panel, uint8_t pot,
                                  const ArcGeometry& geo, uint32_t t_ms);

/* ── SettingsSlot (POD) ─────────────────────────────────────────────── */

struct SettingsSlot
{
    SettingsKind kind = SettingsKind::None;

    /* Catch state — common to every kind that maps a pot to a value. */
    PotState pot = {};

    /* Continuous-value kinds (Brightness / Knob / Bipolar).
     * `value` is the resolved logical value in the kind's natural range:
     *   Brightness — [range_lo .. range_hi]
     *   Knob       — [0..1]
     *   Bipolar    — [-1..+1]                                            */
    float value    = 0.5f;
    float range_lo = 0.0f;
    float range_hi = 1.0f;

    /* Visuals shared across Brightness/Knob/Bipolar. */
    LedPanel::Rgb color        = {0xFF, 0xC0, 0x40};
    LedPanel::Rgb alt_color    = {0x00, 0x00, 0x00};
    LedPanel::Rgb center_color = {0xFF, 0xFF, 0xFF};
    FillAnim      anim         = FillAnim::None;

    /* Selector. */
    uint8_t              num_zones      = 0;
    uint8_t              value_idx      = 0;
    const LedPanel::Rgb* zone_colors    = nullptr;  /* optional palette */
    LedPanel::Rgb        inactive_color = {0x40, 0x40, 0x40};
    float                inactive_dim   = 0.15f;
    ZoneGeometry         zone_geo       = ZoneGeometry::Region;

    /* Preset gesture binding — both halves point at the same UI. */
    PresetGestureUi* preset_gesture = nullptr;

    /* Lock bindings (Settings::UseLocks): the resolved selection is
     * pushed on change and on Deserialize — the zone index via
     * SetSyncMode(), the mapped LockExit via SetExitMode(). */
    LockSource* lock_src      = nullptr;
    LockSource* lock_exit_src = nullptr;

    /* Custom. */
    SettingsTickFn   custom_tick   = nullptr;
    SettingsRenderFn custom_render = nullptr;
    void*            custom_ctx    = nullptr;

    /* Host identity + manual prose.  Descriptor-only — never serialized
     * or hashed, so decorating a control cannot invalidate presets.
     * Unset → positional id / kind-derived label, as before. */
    const char*        ident        = nullptr; /* stable field id        */
    const char*        display_name = nullptr; /* host label             */
    const char* const* labels       = nullptr; /* selector zone labels   */
    uint8_t            num_labels   = 0;
    const char*        help         = nullptr; /* markdown               */
    SeeRef             see[kMaxSeeRefs] = {};
    uint8_t            num_see      = 0;
    bool               see_overflow = false;
};

/* ── Builder handles ────────────────────────────────────────────────── */

/** SeeAlso() storage shared by every handle kind. */
inline void AddSlotSee(SettingsSlot& s, const SeeRef& r)
{
    if (s.num_see < kMaxSeeRefs) s.see[s.num_see++] = r;
    else                         s.see_overflow = true;
}

class BrightnessHandle
{
  public:
    BrightnessHandle() = default;
    explicit BrightnessHandle(SettingsSlot* s) : s_(s) {}
    BrightnessHandle(Settings* owner, SettingsSlot* s,
                     uint8_t page, uint8_t pot)
        : s_(s), owner_(owner), page_(page), pot_(pot) {}

    /** Move the control to another (page, pot).  Handles returned by
     *  Settings know their owner and relocate; a default-constructed or
     *  slot-only handle no-ops.  Defined in settings.cpp. */
    BrightnessHandle& At(uint8_t page, uint8_t pot);

    BrightnessHandle& Range(float lo, float hi)
    { if (!s_) return *this; s_->range_lo = lo; s_->range_hi = hi; return *this; }
    BrightnessHandle& Default(float v)
    { if (!s_) return *this; s_->value = v; s_->pot.stored = ValueToPot(v); return *this; }
    BrightnessHandle& Color  (LedPanel::Rgb c)
    { if (!s_) return *this; s_->color = c; return *this; }

    /* Identity + prose (see SettingsSlot).  The SeeRef conversion lets
     * other entities reference this control; referenced controls need
     * .Ident(). */
    BrightnessHandle& Ident(const char* id)
    { if (s_) s_->ident = id; return *this; }
    BrightnessHandle& Name(const char* n)
    { if (s_) s_->display_name = n; return *this; }
    BrightnessHandle& Help(const char* md)
    { if (s_) s_->help = md; return *this; }
    template <typename... Refs>
    BrightnessHandle& SeeAlso(const Refs&... refs)
    { if (s_) (AddSlotSee(*s_, SeeRef(refs)), ...); return *this; }
    operator SeeRef() const { return s_ ? SeeRef(*s_) : SeeRef(); }

    /** Current logical brightness (already mapped to [range_lo..range_hi]). */
    float Value() const { return s_ ? s_->value : 0.0f; }

    /** Read-only access to the underlying slot.  Stable for the lifetime
     *  of the owning Settings instance.  Returns a sentinel slot pointer
     *  if the handle is unbound — callers always get a usable reference. */
    const SettingsSlot& Slot() const { return s_ ? *s_ : kEmptySlot(); }

  private:
    static const SettingsSlot& kEmptySlot()
    {
        static const SettingsSlot kEmpty{};
        return kEmpty;
    }

    float ValueToPot(float v) const
    {
        const float span = s_->range_hi - s_->range_lo;
        if (span <= 0.0f) return 0.5f;
        const float t = (v - s_->range_lo) / span;
        return (t < 0.0f) ? 0.0f : (t > 1.0f ? 1.0f : t);
    }

    SettingsSlot* s_     = nullptr;
    Settings*     owner_ = nullptr;
    uint8_t       page_  = 0;
    uint8_t       pot_   = 0;
};

class KnobHandle
{
  public:
    KnobHandle() = default;
    explicit KnobHandle(SettingsSlot* s) : s_(s) {}

    KnobHandle& Default(float v)
    { if (!s_) return *this; s_->value = v; s_->pot.stored = v; return *this; }
    KnobHandle& Color  (LedPanel::Rgb c)
    { if (!s_) return *this; s_->color = c; return *this; }
    KnobHandle& Anim   (FillAnim a)
    { if (!s_) return *this; s_->anim  = a; return *this; }

    /* Identity + prose — see BrightnessHandle. */
    KnobHandle& Ident(const char* id) { if (s_) s_->ident = id; return *this; }
    KnobHandle& Name (const char* n)  { if (s_) s_->display_name = n; return *this; }
    KnobHandle& Help (const char* md) { if (s_) s_->help = md; return *this; }
    template <typename... Refs>
    KnobHandle& SeeAlso(const Refs&... refs)
    { if (s_) (AddSlotSee(*s_, SeeRef(refs)), ...); return *this; }
    operator SeeRef() const { return s_ ? SeeRef(*s_) : SeeRef(); }

    float Value() const { return s_ ? s_->value : 0.0f; }

    /** Read-only access to the underlying slot — see BrightnessHandle::Slot. */
    const SettingsSlot& Slot() const
    {
        static const SettingsSlot kEmpty{};
        return s_ ? *s_ : kEmpty;
    }

  private:
    SettingsSlot* s_ = nullptr;
};

class BipolarHandle
{
  public:
    BipolarHandle() = default;
    explicit BipolarHandle(SettingsSlot* s) : s_(s) {}

    BipolarHandle& Default    (float v) /* -1..+1 */
    {
        if (!s_) return *this;
        s_->value = v;
        s_->pot.stored = (v + 1.0f) * 0.5f;
        return *this;
    }
    BipolarHandle& Color       (LedPanel::Rgb c)
    { if (!s_) return *this; s_->color        = c; return *this; }
    BipolarHandle& AltColor    (LedPanel::Rgb c)
    { if (!s_) return *this; s_->alt_color    = c; return *this; }
    BipolarHandle& CenterColor (LedPanel::Rgb c)
    { if (!s_) return *this; s_->center_color = c; return *this; }

    /* Identity + prose — see BrightnessHandle. */
    BipolarHandle& Ident(const char* id) { if (s_) s_->ident = id; return *this; }
    BipolarHandle& Name (const char* n)  { if (s_) s_->display_name = n; return *this; }
    BipolarHandle& Help (const char* md) { if (s_) s_->help = md; return *this; }
    template <typename... Refs>
    BipolarHandle& SeeAlso(const Refs&... refs)
    { if (s_) (AddSlotSee(*s_, SeeRef(refs)), ...); return *this; }
    operator SeeRef() const { return s_ ? SeeRef(*s_) : SeeRef(); }

    /** Current logical value in [-1..+1]. */
    float Value() const { return s_ ? s_->value : 0.0f; }

    /**
     * Pointer to the live resolved value (read-only).  Stable for the
     * lifetime of the owning Settings instance.  Intended for surfaces
     * that need to track this value continuously (e.g. `CvMatrix::Channel
     * ::BindAtten`).  Returns nullptr if the handle is unbound.
     */
    const float* ValuePtr() const { return s_ ? &s_->value : nullptr; }

    /** Read-only access to the underlying slot — see BrightnessHandle::Slot. */
    const SettingsSlot& Slot() const
    {
        static const SettingsSlot kEmpty{};
        return s_ ? *s_ : kEmpty;
    }

  private:
    SettingsSlot* s_ = nullptr;
};

class SelectorHandle
{
  public:
    SelectorHandle() = default;
    explicit SelectorHandle(SettingsSlot* s) : s_(s) {}

    SelectorHandle& Default(uint8_t idx)
    {
        if (!s_) return *this;
        if (s_->num_zones > 0u && idx >= s_->num_zones)
            idx = static_cast<uint8_t>(s_->num_zones - 1u);
        s_->value_idx  = idx;
        s_->pot.stored = IdxToPot(idx);
        return *this;
    }
    SelectorHandle& Colors        (const LedPanel::Rgb* palette)
    { if (!s_) return *this; s_->zone_colors    = palette; return *this; }
    SelectorHandle& InactiveColor (LedPanel::Rgb c)
    { if (!s_) return *this; s_->inactive_color = c;       return *this; }
    SelectorHandle& InactiveDim   (float k)
    { if (!s_) return *this; s_->inactive_dim   = k;       return *this; }
    SelectorHandle& Geometry      (ZoneGeometry g)
    { if (!s_) return *this; s_->zone_geo       = g;       return *this; }

    /** Zone labels; count validated against zones at descriptor build. */
    SelectorHandle& Labels(const char* const* labels, uint8_t n)
    { if (!s_) return *this; s_->labels = labels; s_->num_labels = n; return *this; }

    template <size_t N>
    SelectorHandle& Labels(const char* const (&labels)[N])
    { return Labels(labels, static_cast<uint8_t>(N)); }

    /* Identity + prose — see BrightnessHandle. */
    SelectorHandle& Ident(const char* id) { if (s_) s_->ident = id; return *this; }
    SelectorHandle& Name (const char* n)  { if (s_) s_->display_name = n; return *this; }
    SelectorHandle& Help (const char* md) { if (s_) s_->help = md; return *this; }
    template <typename... Refs>
    SelectorHandle& SeeAlso(const Refs&... refs)
    { if (s_) (AddSlotSee(*s_, SeeRef(refs)), ...); return *this; }
    operator SeeRef() const { return s_ ? SeeRef(*s_) : SeeRef(); }

    /** Current selected zone index, 0..num_zones-1. */
    uint8_t Value() const { return s_ ? s_->value_idx : 0u; }

    /**
     * Pointer to the live selector index (read-only).  Stable for the
     * lifetime of the owning Settings instance.  Intended for surfaces
     * that dispatch on this index every frame (e.g. `CvMatrix::Channel
     * ::Bind`).  Returns nullptr if the handle is unbound.
     */
    const uint8_t* IndexPtr() const { return s_ ? &s_->value_idx : nullptr; }

    /** Read-only access to the underlying slot — see BrightnessHandle::Slot. */
    const SettingsSlot& Slot() const
    {
        static const SettingsSlot kEmpty{};
        return s_ ? *s_ : kEmpty;
    }

  private:
    float IdxToPot(uint8_t idx) const
    {
        if (s_->num_zones == 0u) return 0.5f;
        return (static_cast<float>(idx) + 0.5f) / static_cast<float>(s_->num_zones);
    }
    SettingsSlot* s_ = nullptr;
};

class CustomHandle
{
  public:
    CustomHandle() = default;
    explicit CustomHandle(SettingsSlot* s) : s_(s) {}

    CustomHandle& Default(float v)
    { if (!s_) return *this; s_->value = v; s_->pot.stored = v; return *this; }
    CustomHandle& Tick   (SettingsTickFn   fn)
    { if (!s_) return *this; s_->custom_tick   = fn; return *this; }
    CustomHandle& Render (SettingsRenderFn fn)
    { if (!s_) return *this; s_->custom_render = fn; return *this; }
    CustomHandle& Ctx    (void* p)
    { if (!s_) return *this; s_->custom_ctx    = p;  return *this; }

    /* Identity + prose — see BrightnessHandle. */
    CustomHandle& Ident(const char* id) { if (s_) s_->ident = id; return *this; }
    CustomHandle& Name (const char* n)  { if (s_) s_->display_name = n; return *this; }
    CustomHandle& Help (const char* md) { if (s_) s_->help = md; return *this; }
    template <typename... Refs>
    CustomHandle& SeeAlso(const Refs&... refs)
    { if (s_) (AddSlotSee(*s_, SeeRef(refs)), ...); return *this; }
    operator SeeRef() const { return s_ ? SeeRef(*s_) : SeeRef(); }

    float Value() const { return s_ ? s_->value : 0.0f; }
    SettingsSlot& Slot() const { return *s_; }

  private:
    SettingsSlot* s_ = nullptr;
};

} // namespace alchemy
