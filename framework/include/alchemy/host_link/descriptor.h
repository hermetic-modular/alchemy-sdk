/**
 * @file alchemy/host_link/descriptor.h
 * @brief DescriptorBuilder — renders the module's self-description JSON
 *        (see docs/hostlink-protocol.md §5) with drift guards.
 *
 * The builder derives byte offsets from the *same* introspection the
 * surfaces serialize with (Pager page/pot grid, Settings slot kinds), and
 * verifies at every stage that its arithmetic matches SerializedSize()
 * and the Presets Manage() order.  Any mismatch latches an error and
 * Finish() returns 0 — the firmware then simply reports "no descriptor"
 * instead of shipping a wrong one.
 *
 * Usage (app code, once at boot, after controls are declared and defaults
 * seeded, before BootLoad):
 *
 *   alchemy::hostlink::DescriptorBuilder db(buf, sizeof buf, presets);
 *   db.Begin({.id="echoa", .name="Echoa", .fw=..., .git=..., .sdk=..., .board="v1"});
 *   db.BeginPager("perf", pager);
 *   db.PagerField(0, 0, "d1.time", "Time 1", "{\"kind\":\"expTime\",...}");
 *   ...
 *   db.PagerAltMap("global.layout", kAltIds);   // optional
 *   db.EndPager();
 *   db.Opaque("motion", locks, "Motion recording");
 *   db.BeginSettings("settings", settings);
 *   db.SettingsField(0, 0, "routing.mode", "Delay Mode", "{\"kind\":\"enum\",...}");
 *   ...
 *   db.EndSettings();
 *   const uint32_t len = db.Finish();           // 0 on any drift/overflow
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include "alchemy/host_link/json_writer.h"

namespace alchemy {

class Presets;
class Pager;
class Settings;
class Serializable;
class VirtualButton;

namespace hostlink {

/** Wire field types (protocol §5). */
enum class FieldType : uint8_t { F32, Enum };

class DescriptorBuilder
{
  public:
    struct ModuleInfo
    {
        const char* id;      /* stable machine id, e.g. "echoa"   */
        const char* name;    /* display name                      */
        const char* fw;      /* firmware semver                   */
        const char* git;     /* short git hash                    */
        const char* sdk;     /* SDK version                       */
        const char* board;   /* "v1" / "v2"                       */
    };

    DescriptorBuilder(char* buf, size_t cap, const Presets& presets);

    bool Begin(const ModuleInfo& m);

    /* ── Pager component ────────────────────────────────────────── */
    /**
     * @p page_names / @p page_colors, when non-null, are arrays of
     * exactly NumPages() entries emitted as the component's optional
     * "pageNames" / "pageColors" keys — hosts label and tint the page
     * tabs from them.  Entries may be nullptr (emitted empty; the host
     * falls back to a generic label for that page).  Colors are CSS hex
     * strings, e.g. "#67e8f9".
     */
    bool BeginPager(const char* id, const Pager& pager,
                    const char* const* page_names  = nullptr,
                    const char* const* page_colors = nullptr);
    /** One field per (page, pot); disp_json may be nullptr for a plain
     *  percent readout.  def is captured from the pager's stored value. */
    bool PagerField(uint8_t page, uint8_t pot,
                    const char* field_id, const char* name,
                    const char* disp_json);
    /** Optional alternate position→field mapping (row-major page-major
     *  array of pages×pots field ids), active when the enum field named
     *  @p layout_from selects zone 1.  @p count must equal
     *  NumPages()*NumPots(); a mismatch latches an error and Finish()
     *  returns 0 rather than reading @p field_ids out of bounds. */
    bool PagerAltMap(const char* layout_from, const char* const* field_ids,
                     size_t count);

    template <size_t N>
    bool PagerAltMap(const char* layout_from,
                     const char* const (&field_ids)[N])
    {
        return PagerAltMap(layout_from, field_ids, N);
    }

    bool EndPager();

    /* ── Opaque component (round-trips byte-exact) ──────────────── */
    bool Opaque(const char* id, const Serializable& s, const char* name);

    /* ── Preset-name component (alchemy::PresetName) ────────────────
     * Emits kind "name": a fixed NUL-padded string buffer the host may
     * read and rewrite in place.  Size comes from the component. */
    bool Name(const char* id, const Serializable& s);

    /* ── Generic component ───────────────────────────────────────────
     * For custom Serializables (or hand-rolled overrides of the shaped
     * builders above): any kind string, fields at explicit byte offsets.
     * Hosts render unknown field-bearing kinds as a plain editable group
     * and treat field-less kinds as opaque.  Same drift guards: offsets
     * are validated against the component's SerializedSize() and the
     * component must be the next one in Manage() order. */

    bool BeginComponent(const char* id, const char* kind,
                        const Serializable& s,
                        const char* display_name = nullptr);
    /** Optional "pages"/"pots" grid keys (before the first field). */
    bool ComponentGrid(uint8_t pages, uint8_t pots);
    /** Optional page tab labels/colors (before the first field). */
    bool ComponentPageMeta(const char* const* names,
                           const char* const* colors, uint8_t num_pages);
    /**
     * One field at byte offset @p off within the component.  @p def is
     * the factory default (zone index for Enum).  @p zones is required
     * for Enum fields.  @p page / @p pot are emitted when ≥ 0 (only
     * meaningful for pager/settings-shaped kinds).  @p disp_json may be
     * nullptr for a plain percent/enum readout.
     */
    bool GenericField(const char* id, const char* name, uint32_t off,
                      FieldType type, float def, uint16_t zones,
                      const char* disp_json,
                      int16_t page = -1, int16_t pot = -1);
    bool EndComponent();

    /* ── Settings component ─────────────────────────────────────── */
    /** Page names/colors as in BeginPager (Settings::NumPages() entries). */
    bool BeginSettings(const char* id, const Settings& settings,
                       const char* const* page_names  = nullptr,
                       const char* const* page_colors = nullptr);
    bool SettingsField(uint8_t page, uint8_t pot,
                       const char* field_id, const char* name,
                       const char* disp_json);
    bool EndSettings();

    /* ── Buttons component (kind "buttons", protocol §5.5) ──────────
     * The ButtonBank surface: stateful buttons emit as ordinary 1-byte
     * enum fields (so field-unaware hosts degrade to a plain editable
     * group), each carrying a "btn" object with the physical index and
     * gesture labels; momentary buttons emit into a "modal" array.
     * Modal entries must precede the first field (same ordering rule as
     * GenericMeta).  Offsets are validated against SerializedSize(). */
    bool BeginButtons(const char* id, const Serializable& s);
    bool ButtonsModal(const VirtualButton& b, int16_t page);
    bool ButtonsField(const VirtualButton& b, uint32_t off, int16_t page);
    bool EndButtons();

    /* ── Buttons (top-level, optional) ──────────────────────────────
     * A module's physical push buttons rendered as descriptor metadata
     * alongside the component list.  The array itself is emitted in
     * Finish() so it lands after "components" and before the root
     * close; call this after every EndPager/EndSettings/EndComponent
     * (and before Finish()) to stash the table.  Passing count == 0 is
     * a no-op — the key is omitted entirely, preserving descriptor
     * bytes on modules that expose no button metadata. */
    bool Buttons(const VirtualButton* buttons, uint8_t count);

    template <size_t N>
    bool Buttons(const VirtualButton (&buttons)[N])
    {
        return Buttons(buttons, static_cast<uint8_t>(N));
    }

    /* ── Raw root fragments (top-level, optional) ───────────────────
     * Splice a `"key":value` fragment (no surrounding braces) into the
     * descriptor's root object — the mechanism behind optional root
     * keys like `"storage":{"sd":true,"fsv":1}` (§5.2).  Emitted in
     * Finish() after "buttons"; up to kMaxRootFragments; the pointer
     * must stay valid until Finish().  Malformed fragments fail the
     * final whole-descriptor JSON validation, never ship. */
    bool RawRoot(const char* fragment);

    static constexpr uint8_t kMaxRootFragments = 4u;

    /* ── Generic-component raw key/value ────────────────────────────
     * Emit `,"<key>":<raw_json>` inside the currently-open generic
     * component, between the header (id/kind/hash/size/off) and the
     * "fields" array.  For custom kinds that need per-kind metadata
     * (slot count, per-lock layout, …) without abusing the fields
     * path.  @p raw_json must be a syntactically valid JSON value
     * (object, array, string, number, or bool). */
    bool GenericMeta(const char* key, const char* raw_json);

    bool GenericMetaUInt(const char* key, uint32_t value);

    /** Close all structures and validate totals — including that every
     *  cross-field ref (a button's anchor, a root-array controls entry)
     *  names a field some component emitted; Manage() order is
     *  arbitrary, so neither side can check at emission time.  Returns
     *  descriptor length in bytes, or 0 if anything failed. */
    uint32_t Finish();

    /** Reason for the first failure (static text or a short formatted
     *  message owned by the builder), or nullptr while clean. */
    const char* LastError() const { return err_; }

  private:
    static constexpr uint8_t  kMaxSettingsPages = 4u;
    static constexpr uint8_t  kMaxPots          = 8u;
    static constexpr uint16_t kMaxFieldIds      = 192u;
    static constexpr uint8_t  kMaxRefs          = 24u;

    bool CheckManaged(const Serializable& s);
    void OpenComponent(const char* id, const char* kind,
                       const Serializable& s);
    void EmitPageMeta(const char* const* names, const char* const* colors,
                      uint8_t num_pages);
    void EmitRootButtons();

    bool Fail(const char* msg);
    bool FailRef(const char* kind, const char* target, const char* what);
    bool RecordFieldId(const char* id);
    bool RecordRef(const char* kind, const char* target);

    /** Field ids are hashed, never retained: callers may build an id in
     *  a scratch buffer (the pager's positional "p<page>.<pot>"), so a
     *  stored pointer would dangle by the time Finish() matched it. */
    static uint32_t IdHash(const char* s);

    JsonWriter     w_;
    const Presets& presets_;

    bool     error_       = false;
    uint8_t  comp_index_  = 0u;
    uint32_t cum_off_     = 0u;
    uint32_t comp_size_   = 0u;

    /* Pager context. */
    const Pager* pager_        = nullptr;
    bool         fields_open_  = false;
    bool         generic_open_ = false;

    /* Buttons context. */
    bool buttons_open_ = false;
    bool modal_open_   = false;
    uint32_t buttons_next_off_ = 0u;   /* fields must land densely in order */

    /* Settings context: serialized offset per (page, pot); -1 when the
     * slot persists nothing. */
    const Settings* settings_ = nullptr;
    int16_t         offsets_[kMaxSettingsPages][kMaxPots];

    /* Buttons stashed for emission during Finish().  Pointer is
     * non-owning — the caller keeps the array alive. */
    const VirtualButton* buttons_     = nullptr;
    uint8_t              num_buttons_ = 0u;

    /* Raw root fragments stashed for emission during Finish(). */
    const char* root_fragments_[kMaxRootFragments] = {};
    uint8_t     num_root_fragments_                = 0u;

    /* Ref validation: hashes of every emitted field id, plus every
     * cross-field ref, matched in Finish().  Ref targets come from
     * VirtualButton, whose strings are static by contract, so those
     * pointers are safe to hold for the error message. */
    struct Ref { const char* kind; const char* target; };
    uint32_t field_ids_[kMaxFieldIds] = {};
    uint16_t num_field_ids_           = 0u;
    Ref      refs_[kMaxRefs]          = {};
    uint8_t  num_refs_                = 0u;
    const char* err_                  = nullptr;
    char        errbuf_[96]           = {};
};

} // namespace hostlink
} // namespace alchemy
