/**
 * @file host_link/descriptor.cpp
 * @brief DescriptorBuilder implementation.
 */

#include "alchemy/host_link/descriptor.h"

#include <cstdio>
#include <cstring>

#include "alchemy/host_link/json_check.h"
#include "alchemy/surface/pager.h"
#include "alchemy/surface/presets.h"
#include "alchemy/surface/settings.h"
#include "alchemy/surface/serializable.h"
#include "alchemy/surface/virtual_button.h"

namespace alchemy {
namespace hostlink {

DescriptorBuilder::DescriptorBuilder(char* buf, size_t cap,
                                     const Presets& presets)
    : w_(buf, cap), presets_(presets)
{
    for (uint8_t pg = 0; pg < kMaxSettingsPages; pg++)
        for (uint8_t p = 0; p < kMaxPots; p++)
            offsets_[pg][p] = -1;
}

bool DescriptorBuilder::Begin(const ModuleInfo& m)
{
    w_.BeginObj();
    w_.Key("dv");     w_.UInt(1u);
    w_.Key("module");
    w_.BeginObj();
    w_.Key("id");     w_.Str(m.id);
    w_.Key("name");   w_.Str(m.name);
    w_.Key("fw");     w_.Str(m.fw);
    w_.Key("git");    w_.Str(m.git);
    w_.Key("sdk");    w_.Str(m.sdk);
    w_.Key("board");  w_.Str(m.board);
    w_.EndObj();
    w_.Key("schemaHash"); w_.UInt(presets_.LiveSchemaHash());
    w_.Key("size");       w_.UInt(static_cast<uint32_t>(presets_.LiveSize()));
    w_.Key("components");
    w_.BeginArr();
    return !error_;
}

/* The component now being described must be the next managed component —
 * this pins descriptor order (and offsets) to the real Manage() order. */
bool DescriptorBuilder::CheckManaged(const Serializable& s)
{
    if (presets_.ManagedAt(comp_index_) != &s)
        return Fail("component out of Manage() order");
    return true;
}

/* ── Failure reasons + cross-field ref validation ───────────────────── */

bool DescriptorBuilder::Fail(const char* msg)
{
    if (!err_) err_ = msg;
    error_ = true;
    return false;
}

bool DescriptorBuilder::FailRef(const char* kind, const char* target,
                                const char* what)
{
    if (!err_)
    {
        std::snprintf(errbuf_, sizeof errbuf_, "%s '%s' %s",
                      kind, target, what);
        err_ = errbuf_;
    }
    error_ = true;
    return false;
}

/* FNV-1a: a hash collision would let one dangling ref through, never
 * reject a good one — the safe direction for a placement hint. */
uint32_t DescriptorBuilder::IdHash(const char* s)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s)
    {
        h ^= static_cast<uint8_t>(*s);
        h *= 16777619u;
    }
    return h;
}

bool DescriptorBuilder::RecordFieldId(const char* id)
{
    if (!id) return Fail("field id null");
    if (num_field_ids_ >= kMaxFieldIds)
        return Fail("too many fields for ref validation");
    field_ids_[num_field_ids_++] = IdHash(id);
    return true;
}

bool DescriptorBuilder::RecordRef(const char* kind, const char* target)
{
    if (num_refs_ >= kMaxRefs) return Fail("too many field refs");
    refs_[num_refs_].kind   = kind;
    refs_[num_refs_].target = target;
    num_refs_++;
    return true;
}

void DescriptorBuilder::OpenComponent(const char* id, const char* kind,
                                      const Serializable& s)
{
    comp_size_ = static_cast<uint32_t>(s.SerializedSize());
    w_.BeginObj();
    w_.Key("id");   w_.Str(id);
    w_.Key("kind"); w_.Str(kind);
    w_.Key("hash"); w_.UInt(s.SchemaHash());
    w_.Key("size"); w_.UInt(comp_size_);
    w_.Key("off");  w_.UInt(cum_off_);
}

/* ── Pager ──────────────────────────────────────────────────────────── */

bool DescriptorBuilder::BeginPager(const char* id, const Pager& pager,
                                   const char* const* page_names,
                                   const char* const* page_colors)
{
    if (!CheckManaged(pager)) return false;

    pager_ = &pager;
    OpenComponent(id, "pager", pager);

    /* Drift guard: the field-offset arithmetic below assumes exactly
     * pages × pots × f32. */
    const uint32_t expect = static_cast<uint32_t>(pager.NumPages())
                          * pager.NumPots() * 4u;
    if (expect != comp_size_) return Fail("pager: size != pages*pots*f32");

    w_.Key("pages"); w_.UInt(pager.NumPages());
    w_.Key("pots");  w_.UInt(pager.NumPots());
    EmitPageMeta(page_names, page_colors, pager.NumPages());
    w_.Key("fields");
    w_.BeginArr();
    fields_open_ = true;
    return !error_;
}

/* Optional per-page tab labels/colors ("pageNames" / "pageColors").
 * Null array pointers emit nothing; null entries emit "" so hosts fall
 * back to a generic label for just that page. */
void DescriptorBuilder::EmitPageMeta(const char* const* names,
                                     const char* const* colors,
                                     uint8_t num_pages)
{
    if (names)
    {
        w_.Key("pageNames");
        w_.BeginArr();
        for (uint8_t i = 0; i < num_pages; i++) w_.Str(names[i] ? names[i] : "");
        w_.EndArr();
    }
    if (colors)
    {
        w_.Key("pageColors");
        w_.BeginArr();
        for (uint8_t i = 0; i < num_pages; i++) w_.Str(colors[i] ? colors[i] : "");
        w_.EndArr();
    }
}

bool DescriptorBuilder::PagerField(uint8_t page, uint8_t pot,
                                   const char* field_id, const char* name,
                                   const char* disp_json)
{
    if (!pager_ || !fields_open_) return Fail("pager: field before BeginPager");
    if (page >= pager_->NumPages() || pot >= pager_->NumPots())
        return Fail("pager: field page/pot out of range");
    if (!RecordFieldId(field_id)) return false;

    const uint32_t off =
        (static_cast<uint32_t>(page) * pager_->NumPots() + pot) * 4u;

    w_.BeginObj();
    w_.Key("id");   w_.Str(field_id);
    w_.Key("name"); w_.Str(name);
    w_.Key("off");  w_.UInt(off);
    w_.Key("type"); w_.Str("f32");
    w_.Key("page"); w_.UInt(page);
    w_.Key("pot");  w_.UInt(pot);
    w_.Key("def");  w_.Float(pager_->Stored(page, pot));
    w_.Key("disp");
    if (disp_json) w_.RawValue(disp_json);
    else           w_.RawValue("{\"kind\":\"norm\"}");
    w_.EndObj();
    return !error_;
}

bool DescriptorBuilder::PagerAltMap(const char* layout_from,
                                    const char* const* field_ids,
                                    size_t count)
{
    if (!pager_) return Fail("pager: alt map before BeginPager");

    /* Bound check before any read: the id array must hold exactly one
     * entry per (page, pot).  A mismatch means the alt table drifted from
     * the pager geometry — fail the build instead of over-reading. */
    const size_t expect =
        static_cast<size_t>(pager_->NumPages()) * pager_->NumPots();
    if (count != expect) return Fail("pager: alt map count != pages*pots");

    if (fields_open_) { w_.EndArr(); fields_open_ = false; }

    w_.Key("alt");
    w_.BeginObj();
    w_.Key("layoutFrom"); w_.Str(layout_from);
    w_.Key("pages");
    w_.BeginArr();
    size_t k = 0u;
    for (uint8_t pg = 0; pg < pager_->NumPages(); pg++)
    {
        w_.BeginArr();
        for (uint8_t p = 0; p < pager_->NumPots(); p++)
            w_.Str(field_ids[k++]);
        w_.EndArr();
    }
    w_.EndArr();
    w_.EndObj();
    return !error_;
}

bool DescriptorBuilder::EndPager()
{
    if (!pager_) return Fail("pager: End without Begin");
    if (fields_open_) { w_.EndArr(); fields_open_ = false; }
    w_.EndObj();
    cum_off_ += comp_size_;
    comp_index_++;
    pager_ = nullptr;
    return !error_;
}

/* ── Opaque ─────────────────────────────────────────────────────────── */

bool DescriptorBuilder::Opaque(const char* id, const Serializable& s,
                               const char* name)
{
    if (!CheckManaged(s)) return false;

    OpenComponent(id, "opaque", s);
    w_.Key("name"); w_.Str(name);
    w_.EndObj();
    cum_off_ += comp_size_;
    comp_index_++;
    return !error_;
}

/* ── Preset name ────────────────────────────────────────────────────── */

bool DescriptorBuilder::Name(const char* id, const Serializable& s)
{
    if (!CheckManaged(s)) return false;

    OpenComponent(id, "name", s);
    w_.EndObj();
    cum_off_ += comp_size_;
    comp_index_++;
    return !error_;
}

/* ── Generic component ──────────────────────────────────────────────── */

bool DescriptorBuilder::BeginComponent(const char* id, const char* kind,
                                       const Serializable& s,
                                       const char* display_name)
{
    if (pager_ || settings_ || generic_open_ || buttons_open_)
        return Fail("component: begin while another open");
    if (!CheckManaged(s)) return false;

    OpenComponent(id, kind, s);
    if (display_name) { w_.Key("name"); w_.Str(display_name); }
    generic_open_ = true;
    return !error_;
}

bool DescriptorBuilder::ComponentGrid(uint8_t pages, uint8_t pots)
{
    if (!generic_open_ || fields_open_) return Fail("component: grid after fields");
    w_.Key("pages"); w_.UInt(pages);
    w_.Key("pots");  w_.UInt(pots);
    return !error_;
}

bool DescriptorBuilder::ComponentPageMeta(const char* const* names,
                                          const char* const* colors,
                                          uint8_t num_pages)
{
    if (!generic_open_ || fields_open_) return Fail("component: page meta after fields");
    EmitPageMeta(names, colors, num_pages);
    return !error_;
}

bool DescriptorBuilder::GenericField(const char* id, const char* name,
                                     uint32_t off, FieldType type, float def,
                                     uint16_t zones, const char* disp_json,
                                     int16_t page, int16_t pot)
{
    if (!generic_open_) return Fail("component: field before Begin");
    if (!fields_open_)
    {
        w_.Key("fields");
        w_.BeginArr();
        fields_open_ = true;
    }

    /* Same bound the shaped builders enforce: a field must lie inside
     * the component's serialized bytes. */
    const uint32_t width = (type == FieldType::Enum) ? 1u : 4u;
    if (off + width > comp_size_)
        return Fail("component: field outside serialized bytes");
    if (!RecordFieldId(id)) return false;

    w_.BeginObj();
    w_.Key("id");   w_.Str(id);
    w_.Key("name"); w_.Str(name);
    w_.Key("off");  w_.UInt(off);
    if (page >= 0) { w_.Key("page"); w_.UInt(static_cast<uint32_t>(page)); }
    if (pot  >= 0) { w_.Key("pot");  w_.UInt(static_cast<uint32_t>(pot)); }
    if (type == FieldType::Enum)
    {
        if (zones == 0u) return Fail("component: enum field needs zones");
        w_.Key("type");  w_.Str("enum");
        w_.Key("zones"); w_.UInt(zones);
        w_.Key("def");   w_.UInt(static_cast<uint32_t>(def));
        w_.Key("disp");
        w_.RawValue(disp_json ? disp_json : "{\"kind\":\"enum\"}");
    }
    else
    {
        w_.Key("type"); w_.Str("f32");
        w_.Key("def");  w_.Float(def);
        w_.Key("disp");
        w_.RawValue(disp_json ? disp_json : "{\"kind\":\"norm\"}");
    }
    w_.EndObj();
    return !error_;
}

bool DescriptorBuilder::EndComponent()
{
    if (!generic_open_) return Fail("component: End without Begin");
    if (fields_open_) { w_.EndArr(); fields_open_ = false; }
    w_.EndObj();
    cum_off_ += comp_size_;
    comp_index_++;
    generic_open_ = false;
    return !error_;
}

bool DescriptorBuilder::GenericMeta(const char* key, const char* raw_json)
{
    /* Meta lives between the header and the fields array — after the
     * first Field the writer has already opened "fields":[…] and we
     * cannot inject a sibling key without producing malformed JSON. */
    if (!generic_open_ || fields_open_) return Fail("component: meta after fields");
    /* Reject empty strings too — an empty raw_json emits `"<key>":`
     * with no value, which corrupts the object (subsequent siblings
     * come out with a leading comma).  Callers must pass a full JSON
     * value; there is no valid empty representation. */
    if (!key || !key[0] || !raw_json || !raw_json[0])
        return Fail("component: meta key/value empty");
    if (!ValidJsonValue(raw_json))
        return Fail("component: meta not valid JSON");
    w_.Key(key);
    w_.RawValue(raw_json);
    return !error_;
}

bool DescriptorBuilder::GenericMetaUInt(const char* key, uint32_t value)
{
    if (!generic_open_ || fields_open_) return Fail("component: meta after fields");
    if (!key || !key[0]) return Fail("component: meta key empty");
    w_.Key(key);
    w_.UInt(value);
    return !error_;
}

/* ── Buttons component (kind "buttons") ─────────────────────────────── */

bool DescriptorBuilder::BeginButtons(const char* id, const Serializable& s)
{
    if (pager_ || settings_ || generic_open_ || buttons_open_)
        return Fail("buttons: begin while another open");
    if (!CheckManaged(s)) return false;

    OpenComponent(id, "buttons", s);
    buttons_open_     = true;
    buttons_next_off_ = 0u;
    return !error_;
}

/* Effective field id: the declared Ident(), or the positional "b<hw>"
 * fallback (must match ButtonBank's derivation). */
static const char* ButtonEffectiveId(const VirtualButton& b, char* buf,
                                     size_t cap)
{
    if (b.Ident()) return b.Ident();
    std::snprintf(buf, cap, "b%u", b.HwIndex());
    return buf;
}

/* Derive a host label for one gesture from the button's declaration:
 * explicit label wins; else "Cycle LP / BP / HP" / "Toggle On / Off" /
 * "Set <zone label>" from the zone labels; else just the verb. */
static const char* DeriveGestureLabel(const VirtualButton& b,
                                      const VirtualButton::Gesture& g,
                                      char* buf, size_t cap)
{
    if (g.label) return g.label;
    if (g.fn)    return "";

    const bool toggle = (g.action == VirtualButton::Action::Toggle)
                        || (g.action == VirtualButton::Action::Cycle
                            && b.Zones() == 2u);

    size_t n = 0;
    auto append = [&](const char* s)
    {
        while (*s && n + 1u < cap) buf[n++] = *s++;
    };

    if (g.action == VirtualButton::Action::Set)
    {
        append("Set");
        if (b.ZoneLabels() && g.set_zone < b.NumZoneLabels())
        {
            append(" ");
            append(b.ZoneLabels()[g.set_zone]);
        }
        buf[n] = '\0';
        return buf;
    }

    append(toggle ? "Toggle" : "Cycle");
    if (b.ZoneLabels())
    {
        for (uint8_t i = 0; i < b.NumZoneLabels(); i++)
        {
            append(i == 0u ? " " : " / ");
            append(b.ZoneLabels()[i]);
        }
    }
    buf[n] = '\0';
    return buf;
}

static void EmitGestureEntry(JsonWriter& w, const char* gesture,
                             const char* label)
{
    w.BeginObj();
    w.Key("gesture"); w.Str(gesture);
    w.Key("label");   w.Str(label ? label : "");
    w.EndObj();
}

/* The declared gestures as a JSON array: the structured tap/hold slots
 * (labels derived when not given) plus any legacy Action() metadata. */
static void EmitGestures(JsonWriter& w, const VirtualButton& b,
                         bool implicit_tap)
{
    char buf[96];
    w.BeginArr();
    if (b.TapGesture().used)
        EmitGestureEntry(w, "tap",
                         DeriveGestureLabel(b, b.TapGesture(), buf, sizeof buf));
    else if (implicit_tap)
    {
        VirtualButton::Gesture g;
        g.used = true;
        EmitGestureEntry(w, "tap", DeriveGestureLabel(b, g, buf, sizeof buf));
    }
    if (b.HoldGesture().used)
        EmitGestureEntry(w, "hold",
                         DeriveGestureLabel(b, b.HoldGesture(), buf, sizeof buf));
    for (uint8_t i = 0; i < b.NumActions(); i++)
        EmitGestureEntry(w, b.ActionGesture(i) ? b.ActionGesture(i) : "",
                         b.ActionLabel(i));
    w.EndArr();
}

bool DescriptorBuilder::ButtonsModal(const VirtualButton& b, int16_t page)
{
    /* Modal entries precede the fields array — same JSON-shape rule as
     * GenericMeta. */
    if (!buttons_open_ || fields_open_) return Fail("buttons: modal after fields");
    if (!modal_open_)
    {
        w_.Key("modal");
        w_.BeginArr();
        modal_open_ = true;
    }
    char idbuf[8];
    w_.BeginObj();
    w_.Key("id");   w_.Str(ButtonEffectiveId(b, idbuf, sizeof idbuf));
    w_.Key("name"); w_.Str(b.Name() ? b.Name() : "");
    if (b.HasHw()) { w_.Key("index"); w_.UInt(b.HwIndex()); }
    if (page >= 0) { w_.Key("page");  w_.UInt(static_cast<uint32_t>(page)); }
    w_.Key("gestures");
    EmitGestures(w_, b, false);
    w_.EndObj();
    return !error_;
}

bool DescriptorBuilder::ButtonsField(const VirtualButton& b, uint32_t off,
                                     int16_t page)
{
    if (!buttons_open_) return Fail("buttons: field before BeginButtons");
    if (modal_open_)
    {
        w_.EndArr();
        modal_open_ = false;
    }
    if (!fields_open_)
    {
        w_.Key("fields");
        w_.BeginArr();
        fields_open_ = true;
    }

    /* One byte per stateful button, dense, in roster order. */
    if (b.Zones() < 2u || off != buttons_next_off_
        || off + 1u > comp_size_)
        return Fail("buttons: field not a dense 1-byte enum");
    buttons_next_off_++;

    char idbuf[8];
    const char* eff = ButtonEffectiveId(b, idbuf, sizeof idbuf);
    if (!RecordFieldId(eff)) return false;
    if (b.AnchorField())
    {
        if (std::strcmp(b.AnchorField(), eff) == 0)
            return FailRef("anchor", b.AnchorField(), "is the field itself");
        if (!RecordRef("anchor", b.AnchorField())) return false;
    }

    w_.BeginObj();
    w_.Key("id");   w_.Str(eff);
    w_.Key("name"); w_.Str(b.Name() ? b.Name() : "");
    w_.Key("off");  w_.UInt(off);
    if (page >= 0) { w_.Key("page"); w_.UInt(static_cast<uint32_t>(page)); }
    w_.Key("type");  w_.Str("enum");
    w_.Key("zones"); w_.UInt(b.Zones());
    w_.Key("def");   w_.UInt(b.DefaultZone());

    w_.Key("disp");
    if (b.DispJson())
    {
        w_.RawValue(b.DispJson());
    }
    else if (b.ZoneLabels())
    {
        w_.BeginObj();
        w_.Key("kind"); w_.Str("enum");
        w_.Key("labels");
        w_.BeginArr();
        for (uint8_t i = 0; i < b.NumZoneLabels(); i++)
            w_.Str(b.ZoneLabels()[i] ? b.ZoneLabels()[i] : "");
        w_.EndArr();
        w_.EndObj();
    }
    else
    {
        w_.RawValue("{\"kind\":\"enum\"}");
    }

    if (b.AnchorField()) { w_.Key("anchor"); w_.Str(b.AnchorField()); }

    if (b.HasHw())
    {
        const bool implicit_tap = !b.TapGesture().used
                                  && !b.HoldGesture().used;

        w_.Key("btn");
        w_.BeginObj();
        w_.Key("index"); w_.UInt(b.HwIndex());

        /* Primary action wire-name: the tap mutation (implicit Cycle
         * reads as "toggle" on two-zone buttons).  A callback tap
         * mutates nothing — the key is omitted. */
        const VirtualButton::Gesture& tap = b.TapGesture();
        if (implicit_tap || (tap.used && !tap.fn))
        {
            enum VirtualButton::Action act =
                tap.used ? tap.action : VirtualButton::Action::Cycle;
            if (act == VirtualButton::Action::Cycle && b.Zones() == 2u)
                act = VirtualButton::Action::Toggle;
            w_.Key("action");
            w_.Str(VirtualButton::ActionName(act));
        }
        w_.Key("gestures");
        EmitGestures(w_, b, implicit_tap);
        w_.EndObj();
    }
    w_.EndObj();
    return !error_;
}

bool DescriptorBuilder::EndButtons()
{
    if (!buttons_open_) return Fail("buttons: End without Begin");
    if (modal_open_)  { w_.EndArr(); modal_open_  = false; }
    if (fields_open_) { w_.EndArr(); fields_open_ = false; }
    if (buttons_next_off_ != comp_size_)
        Fail("buttons: size != stateful count");
    w_.EndObj();
    cum_off_ += comp_size_;
    comp_index_++;
    buttons_open_ = false;
    return !error_;
}

/* ── Settings ───────────────────────────────────────────────────────── */

bool DescriptorBuilder::BeginSettings(const char* id, const Settings& settings,
                                      const char* const* page_names,
                                      const char* const* page_colors)
{
    if (!CheckManaged(settings)) return false;

    settings_ = &settings;
    OpenComponent(id, "settings", settings);
    EmitPageMeta(page_names, page_colors, settings.NumPages());

    /* Gesture-owned pots (the UsePresets save/load surface) persist no
     * bytes, so they can never be SettingsFields — but they ARE part of
     * the panel the user sees.  Emit them as display-only metadata so a
     * host can mirror the module's settings pages exactly instead of
     * rendering the preset pots as unexplained gaps.  Hosts that predate
     * the key ignore it (unknown keys are tolerated by protocol rule). */
    bool gestures_open = false;
    for (uint8_t pg = 0; pg < settings.NumPages() && pg < kMaxSettingsPages; pg++)
    {
        for (uint8_t p = 0; p < kMaxPots; p++)
        {
            const SettingsKind k = settings.KindAt(pg, p);
            const char* gname =
                (k == SettingsKind::PresetSlotPot)   ? "Preset Slot"
              : (k == SettingsKind::PresetActionPot) ? "Save / Load"
                                                     : nullptr;
            if (!gname) continue;
            if (!gestures_open)
            {
                w_.Key("gestures");
                w_.BeginArr();
                gestures_open = true;
            }
            w_.BeginObj();
            w_.Key("page"); w_.UInt(pg);
            w_.Key("pot");  w_.UInt(p);
            w_.Key("name"); w_.Str(gname);
            w_.EndObj();
        }
    }
    if (gestures_open) w_.EndArr();

    /* Walk (page, pot) in the exact Serialize() order, accumulating the
     * per-slot byte offsets.  The final total must equal the component's
     * own SerializedSize() — the strongest drift guard available. */
    uint32_t off = 0u;
    for (uint8_t pg = 0; pg < settings.NumPages() && pg < kMaxSettingsPages; pg++)
    {
        for (uint8_t p = 0; p < kMaxPots; p++)
        {
            const size_t n = Settings::PersistedBytesFor(settings.KindAt(pg, p));
            offsets_[pg][p] = (n > 0u) ? static_cast<int16_t>(off) : -1;
            off += static_cast<uint32_t>(n);
        }
    }
    if (off != comp_size_) return Fail("settings: size != slot walk");

    w_.Key("fields");
    w_.BeginArr();
    fields_open_ = true;
    return !error_;
}

bool DescriptorBuilder::SettingsField(uint8_t page, uint8_t pot,
                                      const char* field_id, const char* name,
                                      const char* disp_json)
{
    if (!settings_ || !fields_open_) return Fail("settings: field before BeginSettings");
    if (page >= kMaxSettingsPages || pot >= kMaxPots)
        return Fail("settings: page/pot out of range");
    if (offsets_[page][pot] < 0) return Fail("settings: slot not persisted");
    if (!RecordFieldId(field_id)) return false;

    const SettingsKind kind = settings_->KindAt(page, pot);

    w_.BeginObj();
    w_.Key("id");   w_.Str(field_id);
    w_.Key("name"); w_.Str(name);
    w_.Key("off");  w_.UInt(static_cast<uint32_t>(offsets_[page][pot]));
    w_.Key("page"); w_.UInt(page);
    w_.Key("pot");  w_.UInt(pot);

    if (kind == SettingsKind::Selector)
    {
        w_.Key("type");  w_.Str("enum");
        w_.Key("zones"); w_.UInt(settings_->ZonesAt(page, pot));
        w_.Key("def");   w_.UInt(settings_->SelectorIdxAt(page, pot));
        w_.Key("disp");
        if (disp_json) w_.RawValue(disp_json);
        else           w_.RawValue("{\"kind\":\"enum\"}");
    }
    else
    {
        w_.Key("type"); w_.Str("f32");
        w_.Key("def");  w_.Float(settings_->StoredNormAt(page, pot));
        w_.Key("disp");
        if (disp_json)
        {
            w_.RawValue(disp_json);
        }
        else if (kind == SettingsKind::Brightness)
        {
            w_.BeginObj();
            w_.Key("kind"); w_.Str("brightness");
            w_.Key("lo");   w_.Float(settings_->RangeLoAt(page, pot));
            w_.Key("hi");   w_.Float(settings_->RangeHiAt(page, pot));
            w_.EndObj();
        }
        else if (kind == SettingsKind::Bipolar)
        {
            w_.RawValue("{\"kind\":\"bipolar\"}");
        }
        else
        {
            w_.RawValue("{\"kind\":\"norm\"}");
        }
    }
    w_.EndObj();
    return !error_;
}

bool DescriptorBuilder::EndSettings()
{
    if (!settings_) return Fail("settings: End without Begin");
    if (fields_open_) { w_.EndArr(); fields_open_ = false; }
    w_.EndObj();
    cum_off_ += comp_size_;
    comp_index_++;
    settings_ = nullptr;
    return !error_;
}

/* ── Raw root fragments (top-level) ────────────────────────────────── */

bool DescriptorBuilder::RawRoot(const char* fragment)
{
    if (pager_ || settings_ || generic_open_ || buttons_open_
        || fragment == nullptr || fragment[0] == '\0'
        || num_root_fragments_ >= kMaxRootFragments)
        return Fail("root fragment invalid or too many");
    root_fragments_[num_root_fragments_++] = fragment;
    return true;
}

/* ── Buttons (top-level) ───────────────────────────────────────────── */

bool DescriptorBuilder::Buttons(const VirtualButton* buttons, uint8_t count)
{
    if (pager_ || settings_ || generic_open_ || buttons_open_)
        return Fail("buttons table while component open");
    /* Passing a null pointer with a nonzero count would emit dangling
     * commas in Finish(); reject up front. */
    if (count > 0u && !buttons)
        return Fail("buttons table null with nonzero count");
    buttons_     = buttons;
    num_buttons_ = count;
    return !error_;
}

/* Emit the stashed button table as a top-level "buttons":[...] array.
 * Called from Finish() between the components array close and the root
 * object close.  No-op when the caller stashed nothing.  Controls
 * entries are cross-field refs — collected for the Finish() match. */
void DescriptorBuilder::EmitRootButtons()
{
    if (num_buttons_ == 0u || buttons_ == nullptr) return;
    w_.Key("buttons");
    w_.BeginArr();
    for (uint8_t i = 0; i < num_buttons_; i++)
    {
        const VirtualButton& b = buttons_[i];
        w_.BeginObj();
        w_.Key("id");   w_.Str(b.Ident() ? b.Ident() : "");
        w_.Key("name"); w_.Str(b.Name()  ? b.Name()  : "");
        w_.Key("role"); w_.Str(VirtualButton::RoleName(b.RoleValue()));

        if (b.NumActions() > 0u)
        {
            w_.Key("actions");
            w_.BeginArr();
            for (uint8_t j = 0; j < b.NumActions(); j++)
            {
                w_.BeginObj();
                w_.Key("gesture"); w_.Str(b.ActionGesture(j) ? b.ActionGesture(j) : "");
                w_.Key("label");   w_.Str(b.ActionLabel  (j) ? b.ActionLabel  (j) : "");
                w_.EndObj();
            }
            w_.EndArr();
        }
        if (b.NumControls() > 0u)
        {
            w_.Key("controls");
            w_.BeginArr();
            for (uint8_t j = 0; j < b.NumControls(); j++)
            {
                const char* f = b.ControlField(j);
                w_.BeginObj();
                w_.Key("field");  w_.Str(f ? f : "");
                if (f && f[0]) RecordRef("controls", f);
                w_.Key("action"); w_.Str(VirtualButton::ActionName(b.ControlAction(j)));
                w_.EndObj();
            }
            w_.EndArr();
        }
        w_.EndObj();
    }
    w_.EndArr();
}

/* ── Finish ─────────────────────────────────────────────────────────── */

uint32_t DescriptorBuilder::Finish()
{
    w_.EndArr();   /* components */
    EmitRootButtons();
    for (uint8_t i = 0u; i < num_root_fragments_; i++)
        w_.RawValue(root_fragments_[i]);
    w_.EndObj();   /* root */

    /* Cross-field refs match against the full id table only now — a
     * ref may precede its target in Manage() order or follow it. */
    for (uint8_t i = 0u; i < num_refs_; i++)
    {
        const uint32_t want  = IdHash(refs_[i].target);
        bool           found = false;
        for (uint16_t k = 0u; !found && k < num_field_ids_; k++)
            found = field_ids_[k] == want;
        if (!found) FailRef(refs_[i].kind, refs_[i].target, "names no field");
    }

    if (error_) return 0u;
    if (comp_index_ != presets_.NumManaged())
    {
        Fail("components != Manage() count");
        return 0u;
    }
    if (cum_off_ != presets_.LiveSize())
    {
        Fail("total size drift vs Manage() layout");
        return 0u;
    }
    if (!w_.Ok())
    {
        Fail("descriptor buffer overflow");
        return 0u;
    }
    if (!ValidJsonValue(w_.Data(), w_.Length()))
    {
        Fail("descriptor JSON invalid");
        return 0u;
    }
    return static_cast<uint32_t>(w_.Length());
}

} // namespace hostlink
} // namespace alchemy
