/**
 * @file alchemy/surface/presets.h
 * @brief alchemy::Presets — opt-in preset save/load store with managed
 *        Serializable components.
 *
 * Save / Load walks an ordered list of Serializable* (registered via
 * Manage()) and concatenates their bytes into a single flash slot.
 * The registration order IS the on-flash byte layout.
 *
 * The user does not normally write a Capture/Apply pair: every framework
 * surface they already constructed (Pager, ParamLock<N>, Settings) is a
 * Serializable, and any extra app state can ride along by inheriting
 * Serializable and calling Manage() on it.
 *
 * Slot-level schema gating: every slot is stamped with the XOR of every
 * managed component's SchemaHash().  On Load, a mismatch is treated as
 * an empty slot rather than risking a misaligned restore.  This means
 * adding/removing a managed component, or resizing one (e.g. moving
 * from ParamLock<6> to ParamLock<12>) invalidates existing slots — they
 * disappear instead of corrupting your state.
 *
 * App extras: if you have state outside the standard surfaces, derive
 * a tiny struct from alchemy::Serializable that wraps it, and Manage()
 * the struct.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include "alchemy/control/preset_store.h"
#include "alchemy/hw/alchemy_lab_layout.h"  /* PresetFlashOps, kPresetFlashBase, etc. */
#include "alchemy/surface/preset_name.h"
#include "alchemy/surface/serializable.h"

namespace daisy { class QSPIHandle; }

namespace alchemy {

/* ── Flash blob sizing ───────────────────────────────────────────────── */

/**
 * Bytes available per slot for the concatenated managed-component payload.
 *
 * Computed from the Alchemy Lab preset region constants minus the on-flash
 * record header (20 B) and our own blob preamble (8 B).  In practice a
 * full Pager + ParamLock<16> + Settings load uses ~17 KiB; we cap a few
 * KiB below the physical maximum to leave room for future surfaces.
 *
 */
constexpr size_t kPresetBlobCapacity =
    static_cast<size_t>(kPresetSectorSize) * kPresetSectorsPerSide
    - 32u;  /* RecordHeader (20) + blob preamble (8) + 4-byte safety margin */

/** Maximum number of Serializables that can be registered via Manage(). */
constexpr uint8_t kPresetMaxManaged = 8u;

/* ── Presets ────────────────────────────────────────────────────────── */

class Presets
{
  public:
    static constexpr uint8_t kNumSlots = kPresetNumSlots;

    /**
     * @param qspi  Daisy QSPI handle.  Not used until Init().
     */
    explicit Presets(daisy::QSPIHandle& qspi);

    /**
     * Register a component for save/load.  Order matters — it defines
     * the on-flash byte layout and contributes to the slot schema hash.
     *
     * Call before Init() (or any Save/Load).  Up to kPresetMaxManaged
     * components; excess registrations are silently dropped.
     */
    void Manage(Serializable& s);

    /**
     * Scan slot headers and prepare the store for Save/Load.  Must be
     * called exactly once, after hw.Init() has set up the QSPI XIP
     * mapping.  Safe to call from main() before StartAudio().
     */
    void Init();

    void SetYield(PresetStoreYieldFn fn, void* ctx);

    /** Capture every managed component and write to flash @p slot. */
    bool Save(uint8_t slot);

    /** Restore @p slot's payload into every managed component. */
    bool Load(uint8_t slot);

    /** Restore slot 0 if valid; convenience for boot-time auto-load. */
    bool BootLoad();

    /**
     * Store preset display names inside the blob itself, so names travel
     * with the hardware and inside exports (hosts read and edit them via
     * the descriptor's "name" component).  The owned PresetName is kept
     * pinned LAST in the manage order no matter when this is called, so
     * no other component's blob offset ever depends on call position.
     * Returns the component (e.g. to seed a factory name).
     */
    PresetName& UseNames();

    /**
     * One-shot hook fired at the top of the next BootLoad(), before the
     * boot preset deserializes.  HostLink registers its descriptor render
     * here so captured defaults are factory defaults; app code normally
     * never calls this.  One slot — later calls replace earlier ones.
     */
    void SetPreBootHook(void (*fn)(void*), void* ctx);

    /**
     * True if @p slot holds a CRC-verified record whose schema hash
     * matches the currently-managed set.
     */
    bool HasValid(uint8_t slot) const;

    /* ── Host access (HostLink) / introspection ─────────────────────
     * Everything below operates on the same managed set and the same
     * wear-levelled store as Save/Load.
     */

    /** Custom-backend Init for host tests or non-default flash regions. */
    void Init(const FlashOps& ops, uintptr_t flash_base);

    /** Combined schema hash of the managed set (what slots are stamped with). */
    uint32_t LiveSchemaHash() const { return SchemaHash(); }

    /** Serialized size of the managed set. */
    size_t LiveSize() const { return ManagedSize(); }

    /** Serialize every managed component into @p out (≤ @p cap bytes).
     *  Returns bytes written, or 0 if cap is too small. */
    size_t SerializeLive(uint8_t* out, size_t cap) const;

    /** Apply a byte stream to the managed set (the Load() path minus
     *  flash).  @p len must equal LiveSize(). */
    bool DeserializeLive(const uint8_t* in, size_t len);

    struct SlotInfo
    {
        bool     valid;         /* CRC-verified record present          */
        bool     schema_match;  /* stored hash == LiveSchemaHash()      */
        uint32_t seq;           /* wear-levelling sequence (0 if none)  */
        uint16_t length;        /* stored payload length (0 if none)    */
        uint32_t schema_hash;   /* stored hash (0 if none)              */
    };
    SlotInfo InfoFor(uint8_t slot) const;

    /**
     * Copy up to @p max_len stored payload bytes from @p offset into
     * @p dst, reading straight from memory-mapped flash.  Returns bytes
     * copied (0 when offset ≥ stored length) or -1 when the slot holds
     * no valid record.  @p schema_out / @p total_out are filled on
     * success when non-null.
     */
    int32_t ReadSlotBytes(uint8_t slot, uint32_t offset,
                          uint8_t* dst, uint16_t max_len,
                          uint32_t* schema_out, uint16_t* total_out) const;

    bool WriteSlotRaw(uint8_t slot, uint32_t schema_hash,
                      const uint8_t* bytes, uint16_t len);

    /** Invalidate @p slot (both ping-pong sides). */
    bool EraseSlot(uint8_t slot);

    uint8_t             NumManaged() const { return num_managed_; }
    const Serializable* ManagedAt(uint8_t i) const
    {
        return (i < num_managed_) ? managed_[i] : nullptr;
    }

  private:
    /* On-flash payload: schema-hash + length-prefixed byte stream. */
    struct PresetBlob
    {
        uint32_t schema_hash;
        uint16_t length;
        uint16_t reserved;
        uint8_t  bytes[kPresetBlobCapacity];
    } __attribute__((packed));
    static_assert(sizeof(PresetBlob)
                  == 8u + kPresetBlobCapacity,
                  "PresetBlob layout drift");

    /** XOR of every managed component's SchemaHash(). */
    uint32_t SchemaHash() const;

    /** Sum of every managed component's SerializedSize(). */
    size_t   ManagedSize() const;

    daisy::QSPIHandle*                qspi_;
    PresetStore<PresetBlob, kNumSlots> store_;
    Serializable*                     managed_[kPresetMaxManaged] = {};
    uint8_t                           num_managed_ = 0;

    /* UseNames() state — see Manage() for the pin-last insertion. */
    PresetName                        name_component_;
    bool                              use_names_ = false;

    void (*pre_boot_fn_)(void*) = nullptr;
    void*  pre_boot_ctx_        = nullptr;
};

} // namespace alchemy
