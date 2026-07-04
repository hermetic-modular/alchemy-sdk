/**
 * @file host_link/descriptor.cpp
 * @brief DescriptorBuilder implementation.
 */

#include "alchemy/host_link/descriptor.h"

#include "alchemy/surface/pager.h"
#include "alchemy/surface/presets.h"
#include "alchemy/surface/settings.h"
#include "alchemy/surface/serializable.h"

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
    {
        error_ = true;
        return false;
    }
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
    if (expect != comp_size_) { error_ = true; return false; }

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
    if (!pager_ || !fields_open_) { error_ = true; return false; }
    if (page >= pager_->NumPages() || pot >= pager_->NumPots())
    {
        error_ = true;
        return false;
    }

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
    if (!pager_) { error_ = true; return false; }

    /* Bound check before any read: the id array must hold exactly one
     * entry per (page, pot).  A mismatch means the alt table drifted from
     * the pager geometry — fail the build instead of over-reading. */
    const size_t expect =
        static_cast<size_t>(pager_->NumPages()) * pager_->NumPots();
    if (count != expect) { error_ = true; return false; }

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
    if (!pager_) { error_ = true; return false; }
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
    if (pager_ || settings_ || generic_open_) { error_ = true; return false; }
    if (!CheckManaged(s)) return false;

    OpenComponent(id, kind, s);
    if (display_name) { w_.Key("name"); w_.Str(display_name); }
    generic_open_ = true;
    return !error_;
}

bool DescriptorBuilder::ComponentGrid(uint8_t pages, uint8_t pots)
{
    if (!generic_open_ || fields_open_) { error_ = true; return false; }
    w_.Key("pages"); w_.UInt(pages);
    w_.Key("pots");  w_.UInt(pots);
    return !error_;
}

bool DescriptorBuilder::ComponentPageMeta(const char* const* names,
                                          const char* const* colors,
                                          uint8_t num_pages)
{
    if (!generic_open_ || fields_open_) { error_ = true; return false; }
    EmitPageMeta(names, colors, num_pages);
    return !error_;
}

bool DescriptorBuilder::GenericField(const char* id, const char* name,
                                     uint32_t off, FieldType type, float def,
                                     uint16_t zones, const char* disp_json,
                                     int16_t page, int16_t pot)
{
    if (!generic_open_) { error_ = true; return false; }
    if (!fields_open_)
    {
        w_.Key("fields");
        w_.BeginArr();
        fields_open_ = true;
    }

    /* Same bound the shaped builders enforce: a field must lie inside
     * the component's serialized bytes. */
    const uint32_t width = (type == FieldType::Enum) ? 1u : 4u;
    if (off + width > comp_size_) { error_ = true; return false; }

    w_.BeginObj();
    w_.Key("id");   w_.Str(id);
    w_.Key("name"); w_.Str(name);
    w_.Key("off");  w_.UInt(off);
    if (page >= 0) { w_.Key("page"); w_.UInt(static_cast<uint32_t>(page)); }
    if (pot  >= 0) { w_.Key("pot");  w_.UInt(static_cast<uint32_t>(pot)); }
    if (type == FieldType::Enum)
    {
        if (zones == 0u) { error_ = true; return false; }
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
    if (!generic_open_) { error_ = true; return false; }
    if (fields_open_) { w_.EndArr(); fields_open_ = false; }
    w_.EndObj();
    cum_off_ += comp_size_;
    comp_index_++;
    generic_open_ = false;
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
    if (off != comp_size_) { error_ = true; return false; }

    w_.Key("fields");
    w_.BeginArr();
    fields_open_ = true;
    return !error_;
}

bool DescriptorBuilder::SettingsField(uint8_t page, uint8_t pot,
                                      const char* field_id, const char* name,
                                      const char* disp_json)
{
    if (!settings_ || !fields_open_) { error_ = true; return false; }
    if (page >= kMaxSettingsPages || pot >= kMaxPots) { error_ = true; return false; }
    if (offsets_[page][pot] < 0) { error_ = true; return false; }  /* not persisted */

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
    if (!settings_) { error_ = true; return false; }
    if (fields_open_) { w_.EndArr(); fields_open_ = false; }
    w_.EndObj();
    cum_off_ += comp_size_;
    comp_index_++;
    settings_ = nullptr;
    return !error_;
}

/* ── Finish ─────────────────────────────────────────────────────────── */

uint32_t DescriptorBuilder::Finish()
{
    w_.EndArr();   /* components */
    w_.EndObj();   /* root */

    if (error_) return 0u;
    if (comp_index_ != presets_.NumManaged()) return 0u;
    if (cum_off_ != presets_.LiveSize()) return 0u;
    if (!w_.Ok()) return 0u;
    return static_cast<uint32_t>(w_.Length());
}

} // namespace hostlink
} // namespace alchemy
