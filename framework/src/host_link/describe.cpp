/**
 * @file host_link/describe.cpp
 * @brief ComponentWriter + RenderDescriptor (descriptor auto-derivation).
 */

#include "alchemy/host_link/describe.h"

#include <cstdio>

#include "alchemy/surface/page.h"
#include "alchemy/surface/pager.h"
#include "alchemy/surface/presets.h"
#include "alchemy/surface/settings.h"
#include "alchemy/surface/virtual_knob.h"

namespace alchemy {
namespace hostlink {

/* ── ComponentWriter ────────────────────────────────────────────────── */

ComponentWriter& ComponentWriter::Ident(const char* id)
{
    if (opened_) ok_ = false;
    else         id_ = id;
    return *this;
}

ComponentWriter& ComponentWriter::Kind(const char* kind)
{
    if (opened_) ok_ = false;
    else         kind_ = kind;
    return *this;
}

ComponentWriter& ComponentWriter::Label(const char* display_name)
{
    if (opened_) ok_ = false;
    else         label_ = display_name;
    return *this;
}

ComponentWriter& ComponentWriter::Grid(uint8_t pages, uint8_t pots)
{
    if (opened_) ok_ = false;
    else { grid_pages_ = pages; grid_pots_ = pots; has_grid_ = true; }
    return *this;
}

ComponentWriter& ComponentWriter::PageMeta(const char* const* names,
                                           const char* const* colors,
                                           uint8_t n)
{
    if (opened_) ok_ = false;
    else { names_ = names; colors_ = colors; n_meta_ = n; }
    return *this;
}

bool ComponentWriter::Open()
{
    if (opened_) return ok_;
    opened_ = true;
    ok_ = db_.BeginComponent(id_, kind_, comp_, label_) && ok_;
    if (ok_ && has_grid_) ok_ = db_.ComponentGrid(grid_pages_, grid_pots_);
    if (ok_ && n_meta_)   ok_ = db_.ComponentPageMeta(names_, colors_, n_meta_);
    return ok_;
}

bool ComponentWriter::Field(const char* id, const char* name, uint32_t off,
                            FieldType type, float def, const char* disp_json,
                            uint16_t zones, int16_t page, int16_t pot)
{
    if (!Open()) return false;
    ok_ = db_.GenericField(id, name, off, type, def, zones, disp_json,
                           page, pot)
          && ok_;
    return ok_;
}

bool ComponentWriter::Meta(const char* key, const char* raw_json)
{
    if (!Open()) return false;
    ok_ = db_.GenericMeta(key, raw_json) && ok_;
    return ok_;
}

bool ComponentWriter::Finalize()
{
    if (!Open()) return false;
    ok_ = db_.EndComponent() && ok_;
    return ok_;
}

/* ── Knob lookup + display-hint derivation ──────────────────────────── */

namespace {

const VirtualKnob* FindKnob(const PageSet* pages, uint8_t pg, uint8_t pot)
{
    if (!pages) return nullptr;
    for (uint8_t i = 0; i < pages->count; i++)
    {
        const Page* p = pages->pages[i];
        if (!p || p->Index() != pg) continue;
        for (uint8_t j = 0; j < p->Count(); j++)
        {
            const VirtualKnob* k = p->At(j);
            if (k && k->Pot() == pot) return k;
        }
    }
    return nullptr;
}

/* Derive a display hint from the knob's declared value transform:
 *   Exp(lo, hi)        → {"kind":"exp","lo":..,"hi":..[,"unit":".."]}
 *   Linear(lo, hi)     → {"kind":"linear",...} (plain 0..1 → percent)
 *   Selector + Labels  → {"kind":"snap","labels":[...]}
 *   Selector unlabeled → {"kind":"linear","lo":0,"hi":zones-1}
 * A raw .Disp(json) wins outright.  Returns nullptr for the plain
 * percent readout (and on buffer overflow — a safe degrade). */
const char* DeriveKnobDisp(const VirtualKnob& k, char* buf, size_t cap)
{
    if (k.DispJson()) return k.DispJson();

    JsonWriter jw(buf, cap);
    switch (k.Transform())
    {
        case VirtualKnob::Xform::Selector:
        {
            const uint8_t zones = static_cast<uint8_t>(k.XformMax());
            if (k.Labels() && k.NumLabels() == zones && zones > 0u)
            {
                jw.BeginObj();
                jw.Key("kind"); jw.Str("snap");
                jw.Key("labels");
                jw.BeginArr();
                for (uint8_t i = 0; i < zones; i++) jw.Str(k.Labels()[i]);
                jw.EndArr();
                jw.EndObj();
            }
            else
            {
                jw.BeginObj();
                jw.Key("kind"); jw.Str("linear");
                jw.Key("lo");   jw.Float(0.f);
                jw.Key("hi");   jw.Float(zones > 0u ? zones - 1.f : 0.f);
                jw.EndObj();
            }
            break;
        }
        case VirtualKnob::Xform::Exp:
        case VirtualKnob::Xform::Linear:
        default:
        {
            const bool exp = (k.Transform() == VirtualKnob::Xform::Exp);
            if (!exp && k.XformMin() == 0.f && k.XformMax() == 1.f
                && !k.Unit())
                return nullptr;   /* stock 0..1 knob → percent readout */
            jw.BeginObj();
            jw.Key("kind"); jw.Str(exp ? "exp" : "linear");
            jw.Key("lo");   jw.Float(k.XformMin());
            jw.Key("hi");   jw.Float(k.XformMax());
            if (k.Unit()) { jw.Key("unit"); jw.Str(k.Unit()); }
            jw.EndObj();
            break;
        }
    }
    if (!jw.Terminate()) return nullptr;
    return buf;
}

/* ── Built-in describers for the standard surfaces ──────────────────── */

bool DescribePager(DescriptorBuilder& db, const Pager& pager,
                   const PageSet* pages, const char* id)
{
    /* Tab labels/colors from the declared Pages (sparse ok). */
    const char* names [Pager::kMaxPages] = {};
    const char* colors[Pager::kMaxPages] = {};
    bool any_name = false, any_color = false;
    if (pages)
    {
        for (uint8_t i = 0; i < pages->count; i++)
        {
            const Page* p = pages->pages[i];
            if (!p || p->Index() >= pager.NumPages()) continue;
            if (p->TabName())  { names [p->Index()] = p->TabName();  any_name  = true; }
            if (p->TabColor()) { colors[p->Index()] = p->TabColor(); any_color = true; }
        }
    }

    bool ok = db.BeginPager(id, pager,
                            any_name  ? names  : nullptr,
                            any_color ? colors : nullptr);

    for (uint8_t pg = 0; ok && pg < pager.NumPages(); pg++)
    {
        for (uint8_t pot = 0; ok && pot < pager.NumPots(); pot++)
        {
            const VirtualKnob* k = FindKnob(pages, pg, pot);

            char idbuf[16], namebuf[16], dispbuf[192];
            const char* fid = (k && k->Ident()) ? k->Ident() : idbuf;
            if (fid == idbuf)
                std::snprintf(idbuf, sizeof idbuf, "p%u.%u", pg, pot);
            const char* nm = (k && k->Name() && k->Name()[0]) ? k->Name()
                                                              : namebuf;
            if (nm == namebuf)
                std::snprintf(namebuf, sizeof namebuf, "P%u K%u",
                              pg + 1u, pot + 1u);

            const char* disp =
                k ? DeriveKnobDisp(*k, dispbuf, sizeof dispbuf) : nullptr;
            ok = db.PagerField(pg, pot, fid, nm, disp);
        }
    }
    return ok && db.EndPager();
}

const char* SettingsKindName(SettingsKind k)
{
    switch (k)
    {
        case SettingsKind::Brightness: return "Brightness";
        case SettingsKind::Knob:       return "Knob";
        case SettingsKind::Bipolar:    return "Amount";
        case SettingsKind::Selector:   return "Mode";
        default:                       return "Setting";
    }
}

bool DescribeSettings(DescriptorBuilder& db, const Settings& st,
                      const char* id)
{
    const char* names[kSettingsMaxPages] = {};
    bool any_name = false;
    for (uint8_t pg = 0; pg < st.NumPages() && pg < kSettingsMaxPages; pg++)
        if (st.PageNameAt(pg)) { names[pg] = st.PageNameAt(pg); any_name = true; }

    bool ok = db.BeginSettings(id, st, any_name ? names : nullptr, nullptr);

    /* Count persisted controls per kind so a unique control gets a plain
     * name ("Brightness") and repeats get position suffixes. */
    uint8_t kind_counts[16] = {};
    for (uint8_t pg = 0; pg < st.NumPages(); pg++)
        for (uint8_t pot = 0; pot < kNumPots; pot++)
        {
            const SettingsKind k = st.KindAt(pg, pot);
            if (Settings::PersistedBytesFor(k) > 0u)
                kind_counts[static_cast<uint8_t>(k) & 0x0Fu]++;
        }

    for (uint8_t pg = 0; ok && pg < st.NumPages(); pg++)
    {
        for (uint8_t pot = 0; ok && pot < kNumPots; pot++)
        {
            const SettingsKind k = st.KindAt(pg, pot);
            if (Settings::PersistedBytesFor(k) == 0u) continue;

            char fid[12], nm[24];
            std::snprintf(fid, sizeof fid, "s%u.%u", pg, pot);
            if (kind_counts[static_cast<uint8_t>(k) & 0x0Fu] > 1u)
                std::snprintf(nm, sizeof nm, "%s %u.%u", SettingsKindName(k),
                              pg + 1u, pot + 1u);
            else
                std::snprintf(nm, sizeof nm, "%s", SettingsKindName(k));

            ok = db.SettingsField(pg, pot, fid, nm,
                                  nullptr /* derive from kind */);
        }
    }
    return ok && db.EndSettings();
}

} // namespace

/* ── RenderDescriptor ───────────────────────────────────────────────── */

uint32_t RenderDescriptor(char* buf, size_t cap,
                          const DescriptorBuilder::ModuleInfo& info,
                          const Presets& presets,
                          const PageSet* pages,
                          const DescribeOverride* overrides,
                          uint8_t num_overrides,
                          const VirtualButton* buttons,
                          uint8_t num_buttons)
{
    DescriptorBuilder db(buf, cap, presets);
    bool ok = db.Begin(info);
    if (ok) ok = db.Buttons(buttons, num_buttons);

    uint8_t n_opaque = 0u;
    for (uint8_t i = 0; ok && i < presets.NumManaged(); i++)
    {
        const Serializable* s = presets.ManagedAt(i);
        if (!s) { ok = false; break; }

        char auto_id[8];
        std::snprintf(auto_id, sizeof auto_id, "c%u", i);

        /* 1. Explicit override. */
        const DescribeOverride* ov = nullptr;
        for (uint8_t j = 0; j < num_overrides; j++)
            if (overrides[j].component == s) { ov = &overrides[j]; break; }
        if (ov)
        {
            ComponentWriter w(db, *s, auto_id);
            ok = ov->fn(w, ov->ctx) && w.Finalize();
            continue;
        }

        /* 2. Standard surfaces. */
        switch (s->DescribeKind())
        {
            case Serializable::DescKind::Pager:
                ok = DescribePager(db, static_cast<const Pager&>(*s), pages,
                                   "pager");
                continue;
            case Serializable::DescKind::Settings:
                ok = DescribeSettings(db, static_cast<const Settings&>(*s),
                                      "settings");
                continue;
            case Serializable::DescKind::Name:
                ok = db.Name("name", *s);
                continue;
            default:
                break;
        }

        /* 3. The component's own Describe(); 4. opaque fallback. */
        ComponentWriter w(db, *s, auto_id);
        if (s->Describe(w))
        {
            ok = w.Finalize();
        }
        else if (w.DidWrite())
        {
            ok = false;   /* wrote, then bailed — half a component */
        }
        else
        {
            char nm[12];
            std::snprintf(nm, sizeof nm, "Data %u", ++n_opaque);
            ok = db.Opaque(auto_id, *s, nm);
        }
    }

    return ok ? db.Finish() : 0u;
}

} // namespace hostlink
} // namespace alchemy
