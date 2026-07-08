/**
 * @file alchemy/host_link/component_writer.h
 * @brief ComponentWriter — a DescriptorBuilder view pinned to ONE
 *        component, handed to Serializable::Describe() overrides and
 *        Host::OnDescribe() hooks.
 *
 * A describer can only write its own component: metadata setters latch
 * until the first Field() (or Finalize()) emits the component, and every
 * offset is validated against the component's SerializedSize() by the
 * underlying builder.  A misuse (metadata after a field, out-of-bounds
 * offset) latches failure — the whole descriptor build then returns 0
 * and the module reports "no descriptor" instead of shipping a wrong one.
 *
 * Typical custom component:
 *
 *   bool Describe(hostlink::ComponentWriter& w) const override
 *   {
 *       w.Field("arp.gate", "Gate", 0, hostlink::FieldType::F32, 0.5f);
 *       w.Field("arp.mode", "Mode", 4, hostlink::FieldType::Enum, 0.f,
 *               "{\"kind\":\"enum\",\"labels\":[\"Up\",\"Down\"]}", 2);
 *       return true;
 *   }
 *
 * Defaults: kind "fields" (hosts render the fields as a plain editable
 * group).  Return false from Describe() — writing nothing — to be
 * treated as an opaque byte blob instead.
 */

#pragma once

#include <cstdint>
#include "alchemy/host_link/descriptor.h"

namespace alchemy {

class Serializable;

namespace hostlink {

class ComponentWriter
{
  public:
    /** Constructed by the descriptor renderer; @p default_id must
     *  outlive the write (the renderer passes a stable buffer). */
    ComponentWriter(DescriptorBuilder& db, const Serializable& component,
                    const char* default_id)
        : db_(db), comp_(component), id_(default_id)
    {
    }

    /* ── Component metadata (before the first Field) ────────────────── */

    /** Stable component id (defaults to a positional "c<n>"). */
    ComponentWriter& Ident(const char* id);

    /** Component kind (defaults to "fields"; "opaque" for a named blob). */
    ComponentWriter& Kind(const char* kind);

    /** Display name — shown on opaque chips and generic group tabs. */
    ComponentWriter& Label(const char* display_name);

    /** Optional pages/pots grid keys (pager-shaped kinds). */
    ComponentWriter& Grid(uint8_t pages, uint8_t pots);

    /** Optional page tab labels/colors (arrays of @p n entries). */
    ComponentWriter& PageMeta(const char* const* names,
                              const char* const* colors, uint8_t n);

    /* ── Fields ─────────────────────────────────────────────────────── */

    /** One field at byte offset @p off; see DescriptorBuilder::GenericField. */
    bool Field(const char* id, const char* name, uint32_t off,
               FieldType type, float def, const char* disp_json = nullptr,
               uint16_t zones = 0u, int16_t page = -1, int16_t pot = -1);

    /**
     * Emit `,"<key>":<raw_json>` inside the component object, between the
     * header (id / kind / hash / size / off / optional name) and the
     * "fields" array.  Custom kinds (e.g. "param_locks") use this for
     * per-kind metadata that doesn't fit the pager/settings shape — slot
     * counts, per-lock layouts, etc.  Call *before* the first Field();
     * once the fields array is open, further meta would produce
     * malformed JSON and this call latches the whole build to failure.
     * @p raw_json must be a syntactically valid JSON value.
     */
    bool Meta(const char* key, const char* raw_json);

    bool MetaUInt(const char* key, uint32_t value);

    /* ── Renderer interface ─────────────────────────────────────────── */

    /** Emit + close the component (renderer calls this, not describers). */
    bool Finalize();

    /** True once anything was emitted (renderer misuse detection). */
    bool DidWrite() const { return opened_; }

    bool Ok() const { return ok_; }

  private:
    bool Open();

    DescriptorBuilder&  db_;
    const Serializable& comp_;

    const char*        id_;
    const char*        kind_   = "fields";
    const char*        label_  = nullptr;
    const char* const* names_  = nullptr;
    const char* const* colors_ = nullptr;
    uint8_t            n_meta_ = 0u;
    uint8_t            grid_pages_ = 0u;
    uint8_t            grid_pots_  = 0u;
    bool               has_grid_   = false;

    bool opened_ = false;
    bool ok_     = true;
};

} // namespace hostlink
} // namespace alchemy
