/**
 * @file surface/presets.cpp
 * @brief alchemy::Presets implementation.
 */

#include "alchemy/surface/presets.h"

#include <cstring>

namespace alchemy {

Presets::Presets(daisy::QSPIHandle& qspi) : qspi_(&qspi) {}

void Presets::Manage(Serializable& s)
{
    if (num_managed_ >= kPresetMaxManaged) return;
    if (use_names_)
    {
        /* The UseNames() component stays pinned last: insert new
         * registrations just below it. */
        managed_[num_managed_]      = managed_[num_managed_ - 1u];
        managed_[num_managed_ - 1u] = &s;
        num_managed_++;
        return;
    }
    managed_[num_managed_++] = &s;
}

PresetName& Presets::UseNames()
{
    if (!use_names_ && num_managed_ < kPresetMaxManaged)
    {
        managed_[num_managed_++] = &name_component_;
        use_names_ = true;
    }
    return name_component_;
}

void Presets::SetPreBootHook(void (*fn)(void*), void* ctx)
{
    pre_boot_fn_  = fn;
    pre_boot_ctx_ = ctx;
}

void Presets::Init()
{
    store_.Init(PresetFlashOps(qspi_),
                kPresetFlashBase, kPresetSectorSize, kPresetSectorsPerSide);
}

void Presets::SetYield(PresetStoreYieldFn fn, void* ctx)
{
    store_.SetYield(fn, ctx);
}

uint32_t Presets::SchemaHash() const
{
    uint32_t h = 0xA1C8E51Du;  /* salt — distinguishes "no Manage() at all" */
    for (uint8_t i = 0; i < num_managed_; i++)
    {
        /* Mix position into the hash so reordering invalidates. */
        h ^= managed_[i]->SchemaHash()
           ^ (static_cast<uint32_t>(i) * 0x9E3779B1u);
    }
    return h;
}

size_t Presets::ManagedSize() const
{
    size_t total = 0;
    for (uint8_t i = 0; i < num_managed_; i++)
        total += managed_[i]->SerializedSize();
    return total;
}

bool Presets::Save(uint8_t slot)
{
    const size_t total = ManagedSize();
    if (total > kPresetBlobCapacity) return false;

    /* Serialize straight into the store's staging buffer — no second
     * ~20 KiB PresetBlob on the stack. */
    PresetBlob& blob = *store_.StagingPayload();
    blob.schema_hash = SchemaHash();
    blob.length      = static_cast<uint16_t>(total);
    blob.reserved    = 0u;

    uint8_t* cursor = blob.bytes;
    for (uint8_t i = 0; i < num_managed_; i++)
    {
        managed_[i]->Serialize(cursor);
        cursor += managed_[i]->SerializedSize();
    }

    return store_.CommitStaged(slot, 8u + static_cast<uint32_t>(total));
}

bool Presets::Load(uint8_t slot)
{
    /* Deserialize straight from the memory-mapped record — a stack
     * PresetBlob here is ~20 KiB, comparable to the whole DTCM stack
     * region (same reasoning as Save's staging reuse). */
    const PresetBlob* blob = store_.Peek(slot);
    if (!blob) return false;
    if (blob->schema_hash != SchemaHash()) return false;
    if (blob->length      != ManagedSize()) return false;

    const uint8_t* cursor = blob->bytes;
    for (uint8_t i = 0; i < num_managed_; i++)
    {
        if (!managed_[i]->Deserialize(cursor)) return false;
        cursor += managed_[i]->SerializedSize();
    }
    return true;
}

bool Presets::BootLoad()
{
    if (pre_boot_fn_)
    {
        /* One-shot: components still hold factory defaults here. */
        void (*fn)(void*) = pre_boot_fn_;
        pre_boot_fn_ = nullptr;
        fn(pre_boot_ctx_);
    }
    return HasValid(0) && Load(0);
}

/* ── Host access (HostLink) ───────────────────────────────────────── */

void Presets::Init(const FlashOps& ops, uintptr_t flash_base)
{
    store_.Init(ops, flash_base, kPresetSectorSize, kPresetSectorsPerSide);
}

size_t Presets::SerializeLive(uint8_t* out, size_t cap) const
{
    const size_t total = ManagedSize();
    if (total > cap) return 0u;

    uint8_t* cursor = out;
    for (uint8_t i = 0; i < num_managed_; i++)
    {
        managed_[i]->Serialize(cursor);
        cursor += managed_[i]->SerializedSize();
    }
    return total;
}

bool Presets::DeserializeLive(const uint8_t* in, size_t len)
{
    if (len != ManagedSize()) return false;

    const uint8_t* cursor = in;
    for (uint8_t i = 0; i < num_managed_; i++)
    {
        if (!managed_[i]->Deserialize(cursor)) return false;
        cursor += managed_[i]->SerializedSize();
    }
    return true;
}

Presets::SlotInfo Presets::InfoFor(uint8_t slot) const
{
    SlotInfo info = {false, false, 0u, 0u, 0u};

    uint32_t          seq  = 0u;
    const PresetBlob* blob = store_.Peek(slot, &seq);
    if (!blob) return info;

    info.valid        = true;
    info.seq          = seq;
    info.length       = blob->length;
    info.schema_hash  = blob->schema_hash;
    info.schema_match = (blob->schema_hash == SchemaHash());
    return info;
}

int32_t Presets::ReadSlotBytes(uint8_t slot, uint32_t offset,
                               uint8_t* dst, uint16_t max_len,
                               uint32_t* schema_out, uint16_t* total_out) const
{
    const PresetBlob* blob = store_.Peek(slot);
    if (!blob) return -1;

    /* Harden against a CRC-valid-but-corrupt length field: never trust
     * blob->length past the physical payload capacity before using it to
     * bound the read below. */
    const uint16_t stored_len =
        (blob->length <= kPresetBlobCapacity) ? blob->length : 0u;

    if (schema_out) *schema_out = blob->schema_hash;
    if (total_out)  *total_out  = stored_len;

    if (offset >= stored_len) return 0;
    uint32_t n = static_cast<uint32_t>(blob->length) - offset;
    if (n > max_len) n = max_len;
    std::memcpy(dst, blob->bytes + offset, n);
    return static_cast<int32_t>(n);
}

bool Presets::WriteSlotRaw(uint8_t slot, uint32_t schema_hash,
                           const uint8_t* bytes, uint16_t len)
{
    if (len > kPresetBlobCapacity) return false;

    /* Assemble straight into the store's staging buffer — no second
     * ~20 KiB PresetBlob (formerly a static scratch).  Single-op-in-
     * flight: HostLink only calls this from the control-loop thread, the
     * same thread as on-device Save(), so the shared staging buffer is
     * never concurrently filled. */
    PresetBlob& blob = *store_.StagingPayload();
    blob.schema_hash = schema_hash;
    blob.length      = len;
    blob.reserved    = 0u;
    std::memcpy(blob.bytes, bytes, len);

    return store_.CommitStaged(slot, 8u + static_cast<uint32_t>(len));
}

bool Presets::EraseSlot(uint8_t slot)
{
    return store_.EraseSlot(slot);
}

bool Presets::HasValid(uint8_t slot) const
{
    /* Schema-hash gate: peek at the memory-mapped record and require a
     * match — no ~20 KiB stack temporary (see Load). */
    const PresetBlob* blob = store_.Peek(slot);
    return blob != nullptr && blob->schema_hash == SchemaHash();
}

} // namespace alchemy
