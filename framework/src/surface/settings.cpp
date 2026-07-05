/**
 * @file surface/settings.cpp
 * @brief alchemy::Settings implementation.
 */

#include "alchemy/surface/settings.h"

#include <cstring>

#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/led/anims/catch_pip.h"
#include "alchemy/surface/pager.h"
#include "alchemy/surface/presets.h"

namespace alchemy {

/* Schema tag — bump if Settings's serialised layout grammar changes. */
static constexpr uint32_t kSettingsSchemaTag = 0x53455430u; /* 'SET0' */

/* ── Constructors ─────────────────────────────────────────────────── */

Settings::Settings(IButton& b2, IButton& b3, uint32_t hold_ms)
    : b2_(&b2), b3_(&b3), hold_ms_(hold_ms)
{
}

Settings::Settings(AlchemyLab& hw, Pager* perf_pager, uint32_t hold_ms)
    : b2_(&hw.buttons[1]),
      b3_(&hw.buttons[2]),
      hold_ms_(hold_ms),
      hw_(&hw),
      perf_pager_(perf_pager),
      b1_(&hw.buttons[0])
{
}

/* ── Standard options ─────────────────────────────────────────────── */

BrightnessHandle Settings::UseBrightness()
{
    EnsurePage(0u);
    SettingsSlot& s = slots_[0][0];
    s.kind     = SettingsKind::Brightness;
    s.range_lo = 0.05f;
    s.range_hi = 1.00f;
    s.value    = 0.30f;
    s.pot.stored = (s.value - s.range_lo) / (s.range_hi - s.range_lo);
    s.pot.caught = true;
    s.color    = {0xFF, 0xC0, 0x40};
    if (hw_) ApplyBrightnessSlot(s);
    return BrightnessHandle(&s);
}

PresetGestureUi& Settings::UsePresets(Presets& store)
{
    EnsurePage(0u);
    preset_gesture_.Bind(store);
    uses_presets_ = true;

    SettingsSlot& slot_pot = slots_[0][preset_slot_pot_];
    slot_pot.kind = SettingsKind::PresetSlotPot;
    slot_pot.preset_gesture = &preset_gesture_;
    slot_pot.pot.stored = 0.0f;
    slot_pot.pot.caught = true;

    SettingsSlot& act_pot = slots_[0][preset_action_pot_];
    act_pot.kind = SettingsKind::PresetActionPot;
    act_pot.preset_gesture = &preset_gesture_;
    act_pot.pot.stored = 0.5f;
    act_pot.pot.caught = true;

    return preset_gesture_;
}

/* ── Builders ─────────────────────────────────────────────────────── */

Settings::PageBuilder Settings::Page(uint8_t idx)
{
    if (idx >= kSettingsMaxPages) idx = kSettingsMaxPages - 1;
    EnsurePage(idx);
    return PageBuilder(*this, idx);
}

BrightnessHandle Settings::PotBuilder::Brightness()
{
    SettingsSlot& s = s_.slots_[page_][pot_];
    s.kind     = SettingsKind::Brightness;
    s.range_lo = 0.05f;
    s.range_hi = 1.00f;
    s.value    = 0.30f;
    s.pot.stored = (s.value - s.range_lo) / (s.range_hi - s.range_lo);
    s.pot.caught = true;
    if (s_.hw_) s_.ApplyBrightnessSlot(s);
    return BrightnessHandle(&s);
}

KnobHandle Settings::PotBuilder::Knob()
{
    SettingsSlot& s = s_.slots_[page_][pot_];
    s.kind  = SettingsKind::Knob;
    s.value = 0.5f;
    s.pot.stored = 0.5f;
    s.pot.caught = true;
    return KnobHandle(&s);
}

BipolarHandle Settings::PotBuilder::Bipolar()
{
    SettingsSlot& s = s_.slots_[page_][pot_];
    s.kind  = SettingsKind::Bipolar;
    s.value = 0.0f;
    s.pot.stored = 0.5f;
    s.pot.caught = true;
    return BipolarHandle(&s);
}

SelectorHandle Settings::PotBuilder::Selector(uint8_t num_zones)
{
    SettingsSlot& s = s_.slots_[page_][pot_];
    s.kind      = SettingsKind::Selector;
    s.num_zones = (num_zones == 0u) ? 1u : num_zones;
    s.value_idx = 0;
    s.pot.stored = 0.5f / static_cast<float>(s.num_zones);
    s.pot.caught = true;
    return SelectorHandle(&s);
}

CustomHandle Settings::PotBuilder::Custom()
{
    SettingsSlot& s = s_.slots_[page_][pot_];
    s.kind = SettingsKind::Custom;
    s.pot.stored = 0.5f;
    s.pot.caught = true;
    return CustomHandle(&s);
}

/* ── Internal helpers ─────────────────────────────────────────────── */

void Settings::EnsurePage(uint8_t page)
{
    if (page >= kSettingsMaxPages) return;
    if (page >= num_pages_) num_pages_ = static_cast<uint8_t>(page + 1u);
}

void Settings::ApplyBrightnessSlot(SettingsSlot& s)
{
    if (!hw_) return;
    /* Map [range_lo..range_hi] then push to the panel. */
    const float span = s.range_hi - s.range_lo;
    const float t    = s.pot.stored;
    const float v    = s.range_lo + ((t < 0.0f) ? 0.0f : (t > 1.0f ? span : t * span));
    s.value = v;
    hw_->leds.SetBrightness(v);
}

void Settings::ArmPage(uint8_t page, const float* phys)
{
    if (page >= num_pages_) return;
    bool has_preset_action = false;
    for (uint8_t p = 0; p < kNumPots; p++)
    {
        SettingsSlot& s = slots_[page][p];
        if (s.kind == SettingsKind::None) continue;
        if (s.kind == SettingsKind::PresetSlotPot
         || s.kind == SettingsKind::PresetActionPot)
        {
            /* Preset pots are gesture-driven; we always treat them as caught
             * so the gesture sees the raw physical pot value immediately. */
            s.pot.stored = phys[p];
            s.pot.caught = true;
            if (s.kind == SettingsKind::PresetActionPot) has_preset_action = true;
            continue;
        }
        InitCatch(s.pot, phys[p]);
    }

    /* Disarm the preset gesture on entry: if the action pot is already
     * parked at a save/load edge, require the user to bring it through
     * neutral (noon) before save/load can re-arm. */
    if (has_preset_action && uses_presets_) preset_gesture_.Disarm();
}

/* ── Mode-flag gesture (unchanged from legacy) ────────────────────── */

void Settings::TickModeGesture(uint32_t t_ms)
{
    const bool b2_pressed = b2_->Pressed();
    const bool b3_pressed = b3_->Pressed();
    const bool b2_rising  = b2_pressed && !prev_b2_pressed_;
    const bool b3_rising  = b3_pressed && !prev_b3_pressed_;
    prev_b2_pressed_ = b2_pressed;
    prev_b3_pressed_ = b3_pressed;

    if (active_)
    {
        if (b2_rising || b3_rising)
        {
            active_           = false;
            both_held_active_ = false;
        }
        return;
    }

    if (b2_pressed && b3_pressed)
    {
        if (!both_held_active_)
        {
            both_held_active_ = true;
            both_held_since_  = t_ms;
        }
        else if ((t_ms - both_held_since_) >= hold_ms_)
        {
            active_           = true;
            both_held_active_ = false;
        }
    }
    else
    {
        both_held_active_ = false;
    }
}

void Settings::PollButtons(uint32_t t_ms)
{
    /* B2+B3 hold-to-enter / B2 or B3 release-to-exit. */
    TickModeGesture(t_ms);
    gesture_polled_ = true;

    /* B1 short-press cycles settings pages while active. Track prev_b1
     * unconditionally so we don't fire a spurious edge on (re)entry. */
    if (!b1_) return;
    const bool b1_pressed = b1_->Pressed();
    const bool b1_falling = !b1_pressed && prev_b1_pressed_;
    prev_b1_pressed_ = b1_pressed;
    if (active_ && b1_falling && num_pages_ > 1u)
        settings_advance_pending_ = true;
}

/* ── Update ───────────────────────────────────────────────────────── */

void Settings::Update(uint32_t t_ms) { Update(nullptr, t_ms); }

void Settings::Update(const float* phys, uint32_t t_ms)
{
    /* Mode gesture and B1 page-nav edge detection are normally done by
     * PollButtons at 1 ms cadence. If PollButtons hasn't fired since the
     * last Update (legacy ctor users or hand-rolled loops not using
     * ControlLoop), fall back to per-frame gesture detection here. */
    if (!gesture_polled_) TickModeGesture(t_ms);
    gesture_polled_ = false;

    /* Active-state transitions are detected against prev_update_active_. */
    const bool was_active = prev_update_active_;
    prev_update_active_   = active_;

    /* Compute dt for preset gesture timing.  First tick after enter
     * gets 0 to avoid a large impulse. */
    const float dt_ms = (last_t_ms_ == 0u || t_ms < last_t_ms_)
                            ? 0.0f
                            : static_cast<float>(t_ms - last_t_ms_);
    last_t_ms_ = t_ms;

    /* Flag-only mode (no hw_ → no controls to drive). */
    if (!hw_ || !phys) return;

    /* Lazy re-arm post-Deserialize — happens regardless of mode so a
     * preset load that happens while we're in perf mode still re-arms
     * the next time we enter settings (or now, if we're active). */
    if (pending_rearm_)
    {
        pending_rearm_ = false;
        for (uint8_t pg = 0; pg < num_pages_; pg++) ArmPage(pg, phys);
        /* Brightness in particular needs to be pushed to the LED panel. */
        for (uint8_t pg = 0; pg < num_pages_; pg++)
            for (uint8_t p = 0; p < kNumPots; p++)
                if (slots_[pg][p].kind == SettingsKind::Brightness)
                    ApplyBrightnessSlot(slots_[pg][p]);
    }

    /* Edge transitions. */
    if (active_ && !was_active) ArmPage(page_, phys);
    if (!active_ && was_active && perf_pager_)
        perf_pager_->LockPage(perf_pager_->Page(), phys);

    if (!active_) return;

    /* Consume the B1 page-cycle flag set by PollButtons. */
    if (settings_advance_pending_)
    {
        settings_advance_pending_ = false;
        page_ = static_cast<uint8_t>((page_ + 1u) % num_pages_);
        ArmPage(page_, phys);
    }

    /* Tick every slot on the active page. */
    for (uint8_t p = 0; p < kNumPots; p++)
    {
        SettingsSlot& s = slots_[page_][p];
        switch (s.kind)
        {
            case SettingsKind::None:
                break;

            case SettingsKind::Brightness:
                UpdateCatch(s.pot, phys[p]);
                ApplyBrightnessSlot(s);
                break;

            case SettingsKind::Knob:
                UpdateCatch(s.pot, phys[p]);
                s.value = s.pot.stored;
                break;

            case SettingsKind::Bipolar:
                UpdateCatch(s.pot, phys[p]);
                s.value = (s.pot.stored - 0.5f) * 2.0f;
                break;

            case SettingsKind::Selector:
            {
                UpdateCatch(s.pot, phys[p]);
                float v = s.pot.stored;
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                int idx = static_cast<int>(v * static_cast<float>(s.num_zones));
                if (idx >= static_cast<int>(s.num_zones)) idx = s.num_zones - 1;
                s.value_idx = static_cast<uint8_t>(idx);
                break;
            }

            case SettingsKind::PresetSlotPot:
            case SettingsKind::PresetActionPot:
                /* No catch — we feed the gesture the live phys value. */
                s.pot.stored = phys[p];
                s.pot.caught = true;
                break;

            case SettingsKind::Custom:
                if (s.custom_tick) s.custom_tick(s, phys[p], t_ms);
                break;
        }
    }

    /* Drive the preset gesture if registered (always on page 0). */
    if (uses_presets_ && page_ == 0u)
    {
        const float slot_v   = phys[preset_slot_pot_];
        const float action_v = phys[preset_action_pot_];
        const bool committed = preset_gesture_.Update(slot_v, action_v, dt_ms);
        if (committed)
        {
            /* Save: nothing else to do.
             * Load: every managed Serializable just changed — set pending_rearm_
             *       so the next Update locks every page against fresh phys. */
            pending_rearm_ = true;
        }
    }
}

/* ── Render ───────────────────────────────────────────────────────── */

void Settings::Render(uint32_t t_ms) const
{
    if (!hw_) return;

    const ArcGeometry& geo = hw_->Arc();

    for (uint8_t p = 0; p < kNumPots; p++)
    {
        const SettingsSlot& s = slots_[page_][p];
        switch (s.kind)
        {
            case SettingsKind::None:
                break;

            case SettingsKind::Brightness:
            {
                const float k = s.pot.caught ? 1.0f : 0.30f;
                FillDesc desc;
                desc.mode  = FillMode::Edge;
                desc.color = LedPanel::Scale(s.color, k);
                DrawFill(hw_->leds, p, geo, s.pot.stored, desc, t_ms);
                DrawCatchPip(hw_->leds, p, s.pot, geo, {0xFF, 0xFF, 0xFF});
                break;
            }

            case SettingsKind::Knob:
            {
                const float k = s.pot.caught ? 1.0f : 0.30f;
                FillDesc desc;
                desc.mode  = FillMode::Edge;
                desc.color = LedPanel::Scale(s.color, k);
                desc.anim  = s.anim;
                DrawFill(hw_->leds, p, geo, s.pot.stored, desc, t_ms);
                DrawCatchPip(hw_->leds, p, s.pot, geo, {0xFF, 0xFF, 0xFF});
                break;
            }

            case SettingsKind::Bipolar:
            {
                const float k = s.pot.caught ? 1.0f : 0.30f;
                FillDesc desc;
                desc.mode          = FillMode::Center;
                desc.color         = LedPanel::Scale(s.color,        k);
                desc.passive_color = LedPanel::Scale(s.alt_color,    k);
                desc.center_color  = LedPanel::Scale(s.center_color, k);
                desc.pivot01       = 0.5f;
                DrawFill(hw_->leds, p, geo, s.value, desc, t_ms);
                DrawCatchPip(hw_->leds, p, s.pot, geo, {0xFF, 0xFF, 0xFF});
                break;
            }

            case SettingsKind::Selector:
            {
                const float k = s.pot.caught ? 1.0f : 0.30f;
                SelectorDesc desc;
                desc.num_zones      = s.num_zones;
                desc.zone_geo       = s.zone_geo;
                desc.inactive_color = s.inactive_color;
                desc.inactive_dim   = s.inactive_dim;
                if (s.zone_colors)
                {
                    desc.active_color =
                        LedPanel::Scale(s.zone_colors[s.value_idx], k);
                }
                else
                {
                    desc.active_color = LedPanel::Scale(s.color, k);
                }
                DrawSelector(hw_->leds, p, geo, s.pot.stored, desc);
                DrawCatchPip(hw_->leds, p, s.pot, geo, {0xFF, 0xFF, 0xFF});
                break;
            }

            case SettingsKind::PresetSlotPot:
            case SettingsKind::PresetActionPot:
                /* The gesture renders both pots together — handled once below. */
                break;

            case SettingsKind::Custom:
                if (s.custom_render) s.custom_render(s, hw_->leds, p, geo, t_ms);
                break;
        }
    }

    /* Preset gesture is a two-pot draw; do it once per frame. */
    if (uses_presets_ && page_ == 0u)
    {
        preset_gesture_.Render(hw_->leds, geo,
                               preset_slot_pot_, preset_action_pot_, t_ms);
    }

    /* Mode indicator on B1. */
    hw_->leds.SetButtonPair(0, indicator_color_);
}

/* ── Serializable ─────────────────────────────────────────────────── */

namespace {

/* Persistent payload kind tag — used both for SchemaHash and to keep
 * the serialised stream agnostic to physical pot positions. */
enum class PersistKind : uint8_t
{
    None = 0,
    Float,    /* 4 bytes — Brightness, Knob, Bipolar via pot.stored */
    Byte,     /* 1 byte  — Selector value_idx */
};

PersistKind PersistKindOf(SettingsKind k)
{
    switch (k)
    {
        case SettingsKind::Brightness:
        case SettingsKind::Knob:
        case SettingsKind::Bipolar:    return PersistKind::Float;
        case SettingsKind::Selector:   return PersistKind::Byte;
        default:                       return PersistKind::None;
    }
}

size_t PersistBytes(PersistKind pk)
{
    switch (pk)
    {
        case PersistKind::Float: return sizeof(float);
        case PersistKind::Byte:  return 1u;
        default:                 return 0u;
    }
}

}  // namespace

size_t Settings::SerializedSize() const
{
    size_t total = 0;
    for (uint8_t pg = 0; pg < num_pages_; pg++)
        for (uint8_t p = 0; p < kNumPots; p++)
            total += PersistBytes(PersistKindOf(slots_[pg][p].kind));
    return total;
}

void Settings::Serialize(uint8_t* out) const
{
    for (uint8_t pg = 0; pg < num_pages_; pg++)
    {
        for (uint8_t p = 0; p < kNumPots; p++)
        {
            const SettingsSlot& s = slots_[pg][p];
            switch (PersistKindOf(s.kind))
            {
                case PersistKind::Float:
                {
                    const float v = s.pot.stored;
                    std::memcpy(out, &v, sizeof(float));
                    out += sizeof(float);
                    break;
                }
                case PersistKind::Byte:
                    out[0] = s.value_idx;
                    out  += 1u;
                    break;
                default:
                    break;
            }
        }
    }
}

bool Settings::Deserialize(const uint8_t* in)
{
    for (uint8_t pg = 0; pg < num_pages_; pg++)
    {
        for (uint8_t p = 0; p < kNumPots; p++)
        {
            SettingsSlot& s = slots_[pg][p];
            switch (PersistKindOf(s.kind))
            {
                case PersistKind::Float:
                {
                    float v;
                    std::memcpy(&v, in, sizeof(float));
                    in += sizeof(float);
                    s.pot.stored = v;
                    /* Resolved logical value re-derives on the next Update. */
                    break;
                }
                case PersistKind::Byte:
                    s.value_idx = in[0];
                    in += 1u;
                    /* Resolved pot.stored re-derives on the next Update. */
                    break;
                default:
                    break;
            }
        }
    }
    pending_rearm_ = true;
    return true;
}

uint32_t Settings::SchemaHash() const
{
    /* Mix in shape: page count, per-(page, pot) kind tag.  Stable across
     * builds when the declarative layout is unchanged. */
    uint32_t h = kSettingsSchemaTag
               ^ (static_cast<uint32_t>(num_pages_) << 8);
    for (uint8_t pg = 0; pg < num_pages_; pg++)
    {
        for (uint8_t p = 0; p < kNumPots; p++)
        {
            const uint32_t mix = (static_cast<uint32_t>(slots_[pg][p].kind) << 16)
                               ^ (static_cast<uint32_t>(pg) << 8)
                               ^ static_cast<uint32_t>(p);
            h = (h * 0x01000193u) ^ mix;   /* FNV-ish mix */
        }
    }
    return h;
}

} // namespace alchemy
