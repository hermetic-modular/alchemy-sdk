/**
 * @file preset_store.h
 * @brief Generic wear-levelled preset store for memory-mapped flash.
 *
 * PresetStore<Payload, kNumSlots> implements a ping-pong wear-levelling
 * scheme on top of any memory-mapped flash device:
 *
 *   Layout:  kNumSlots slots × 2 sides × (sectors_per_side × sector_size) bytes
 *
 *   Each side holds one record:
 *     [RecordHeader (20 bytes)] [Payload (sizeof(Payload) bytes)]
 *
 *   On Save:  the older of the two sides is erased and overwritten with a
 *             new record whose sequence counter is one higher than any
 *             previously seen value.  If one side is invalid (factory blank
 *             or corrupt), that side is preferred so erasure is minimised.
 *
 *   On Load:  the side with the highest valid (CRC-verified) sequence
 *             number is returned.
 *
 * Platform requirements
 * ---------------------
 * The caller supplies a FlashOps struct at Init() with three callbacks:
 *   erase(ctx, start, end)           — erase the range [start, end)
 *   write(ctx, addr, buf, len)       — write len bytes from buf to addr
 *   invalidate(ctx, addr, len)       — invalidate D-cache for [addr, addr+len)
 *                                      (may be a no-op on cacheless targets)
 *
 * Flash must be readable via direct memory-mapped pointer casts (the
 * standard arrangement for QSPI/XIP flash on Cortex-M parts).
 *
 * Usage
 * -----
 *   using MyStore = alchemy::PresetStore<MyData, 16>;
 *   MyStore store;
 *   store.Init(ops, flash_base_addr, sector_size, sectors_per_side);
 *   store.Save(0, data);
 *   store.Load(0, data);
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include "alchemy/flash_ops.h"   /* FlashOps interface (hardware/include/alchemy/) */

namespace alchemy {

using PresetStoreYieldFn = void (*)(void* ctx);

/* ── PresetStore ───────────────────────────────────────────────────── */

template <typename Payload, uint8_t kNumSlots = 16u>
class PresetStore
{
  public:
    /**
     * Scan all slots and hydrate the in-RAM index.
     * Must be called once before HasValid / Load / Save.
     *
     * @param ops              Platform erase / write / cache-invalidate callbacks.
     * @param flash_base       First byte of the preset flash region.
     * @param sector_size      Erase granule in bytes (e.g. 4096 for IS25LP064A).
     * @param sectors_per_side Number of sectors allocated per ping-pong side.
     */
    void Init(const FlashOps& ops,
              uintptr_t flash_base,
              uint32_t sector_size,
              uint8_t  sectors_per_side)
    {
        ops_              = ops;
        flash_base_       = flash_base;
        sector_size_      = sector_size;
        sectors_per_side_ = sectors_per_side;
        next_seq_         = 0u;

        for (uint8_t slot = 0u; slot < kNumSlots; slot++)
        {
            for (uint8_t side = 0u; side < 2u; side++)
            {
                sectors_[slot][side].valid = false;
                sectors_[slot][side].seq   = 0u;

                const uintptr_t addr = SideAddr(slot, side);
                ops_.invalidate(ops_.ctx, addr,
                                static_cast<uint32_t>(sizeof(RecordHeader)
                                                      + sizeof(Payload)));

                const RecordHeader* hdr =
                    reinterpret_cast<const RecordHeader*>(addr);

                if (hdr->magic   != kMagic)              continue;
                if (hdr->version != kVersion)            continue;
                if (hdr->payload_len > sizeof(Payload))  continue;

                const uint8_t* payload =
                    reinterpret_cast<const uint8_t*>(
                        addr + sizeof(RecordHeader));
                if (Crc32(payload, hdr->payload_len) != hdr->crc) continue;

                sectors_[slot][side].valid = true;
                sectors_[slot][side].seq   = hdr->seq;
                if (hdr->seq >= next_seq_)
                    next_seq_ = hdr->seq + 1u;
            }
        }
    }

    /**
     * Optional cooperative-yield hook, called between flash chunks
     * (after each sector erase and each sector-sized write) during
     * Save().  A full side is sectors_per_side × sector-erase — up to
     * several hundred ms of blocking flash time on typical NOR parts —
     * so a host with a watchdog or time-critical polling (CV edges,
     * external clock) should service them here.  The hook runs in the
     * caller's context; it must not re-enter Save()/Load().
     *
     * Survives Init(); pass nullptr to remove.
     */
    void SetYield(PresetStoreYieldFn fn, void* ctx)
    {
        yield_     = fn;
        yield_ctx_ = ctx;
    }

    /** Returns true if @p slot holds a CRC-verified flash record. */
    bool HasValid(uint8_t slot) const
    {
        if (slot >= kNumSlots) return false;
        return sectors_[slot][0].valid || sectors_[slot][1].valid;
    }

    /**
     * Copy the payload from the most-recent valid record into @p out.
     * Returns true on success, false if no valid record exists for @p slot.
     */
    bool Load(uint8_t slot, Payload& out) const
    {
        if (!HasValid(slot)) return false;

        uint8_t  best_side = 0xFFu;
        uint32_t best_seq  = 0u;
        for (uint8_t side = 0u; side < 2u; side++)
        {
            if (!sectors_[slot][side].valid) continue;
            if (best_side == 0xFFu || sectors_[slot][side].seq > best_seq)
            {
                best_side = side;
                best_seq  = sectors_[slot][side].seq;
            }
        }
        if (best_side == 0xFFu) return false;

        const uintptr_t side_addr = SideAddr(slot, best_side);
        ops_.invalidate(ops_.ctx, side_addr,
                        static_cast<uint32_t>(sizeof(RecordHeader)
                                              + sizeof(Payload)));

        const RecordHeader* hdr =
            reinterpret_cast<const RecordHeader*>(side_addr);
        const uint32_t n = (hdr->payload_len <= sizeof(Payload))
                               ? hdr->payload_len
                               : static_cast<uint32_t>(sizeof(Payload));

        std::memset(&out, 0, sizeof(Payload));
        std::memcpy(&out,
                    reinterpret_cast<const void*>(
                        side_addr + sizeof(RecordHeader)),
                    n);
        return true;
    }

    /**
     * Write @p data to the older ping-pong side for @p slot.
     * Returns true on success.
     */
    bool Save(uint8_t slot, const Payload& data)
    {
        std::memcpy(StagingPayload(), &data, sizeof(Payload));
        return CommitStaged(slot, sizeof(Payload));
    }

    Payload* StagingPayload()
    {
        return reinterpret_cast<Payload*>(write_buf_ + sizeof(RecordHeader));
    }

    bool CommitStaged(uint8_t slot, uint32_t used_bytes = sizeof(Payload))
    {
        if (slot >= kNumSlots) return false;
        if (used_bytes > sizeof(Payload)) return false;

        /* Choose target side: prefer invalid sides first; then overwrite
         * the one with the lower sequence number. */
        uint8_t target;
        if      (!sectors_[slot][0].valid)  target = 0u;
        else if (!sectors_[slot][1].valid)  target = 1u;
        else target = (sectors_[slot][0].seq <= sectors_[slot][1].seq)
                      ? 0u : 1u;

        const uintptr_t addr = SideAddr(slot, target);

        /* Only the sectors covering the record are touched. */
        const uint32_t record_len =
            static_cast<uint32_t>(sizeof(RecordHeader)) + used_bytes;
        uint32_t sectors_needed =
            (record_len + sector_size_ - 1u) / sector_size_;
        if (sectors_needed > sectors_per_side_)
            sectors_needed = sectors_per_side_;

        /* Kick/service the host once immediately before the first blocking
         * sector erase, so the longest gap between yields is a single
         * erase (or write), never erase + inter-op overhead.           */
        Yield();

        for (uint32_t s = 0u; s < sectors_needed; s++)
        {
            const uintptr_t s_start = addr + s * sector_size_;
            if (!ops_.erase(ops_.ctx, s_start, s_start + sector_size_))
                return false;
            Yield();
        }

        /* Assemble the header in place ahead of the (already-staged)
         * payload; CRC and payload_len cover exactly the used bytes. */
        const uint8_t* payload = write_buf_ + sizeof(RecordHeader);
        RecordHeader hdr;
        hdr.magic       = kMagic;
        hdr.version     = kVersion;
        hdr.reserved    = 0u;
        hdr.seq         = next_seq_;
        hdr.payload_len = used_bytes;
        hdr.crc         = Crc32(payload, used_bytes);

        std::memcpy(write_buf_, &hdr, sizeof(RecordHeader));

        for (uint32_t off = 0u; off < record_len; off += sector_size_)
        {
            const uint32_t len = (record_len - off < sector_size_)
                                     ? (record_len - off) : sector_size_;
            if (!ops_.write(ops_.ctx, addr + off, write_buf_ + off, len))
                return false;
            Yield();
        }

        ops_.invalidate(ops_.ctx, addr, record_len);

        sectors_[slot][target].valid = true;
        sectors_[slot][target].seq   = hdr.seq;
        next_seq_++;
        return true;
    }

    const Payload* Peek(uint8_t slot, uint32_t* seq_out = nullptr) const
    {
        if (slot >= kNumSlots) return nullptr;

        uint8_t  best_side = 0xFFu;
        uint32_t best_seq  = 0u;
        for (uint8_t side = 0u; side < 2u; side++)
        {
            if (!sectors_[slot][side].valid) continue;
            if (best_side == 0xFFu || sectors_[slot][side].seq > best_seq)
            {
                best_side = side;
                best_seq  = sectors_[slot][side].seq;
            }
        }
        if (best_side == 0xFFu) return nullptr;

        const uintptr_t payload_addr =
            SideAddr(slot, best_side)
            + static_cast<uintptr_t>(sizeof(RecordHeader));
        ops_.invalidate(ops_.ctx, payload_addr,
                        static_cast<uint32_t>(sizeof(Payload)));
        if (seq_out) *seq_out = best_seq;
        return reinterpret_cast<const Payload*>(payload_addr);
    }

    /**
     * Invalidate @p slot by erasing the header sector of both sides.
     * Cheap (2 sector erases vs. a full side rewrite) and safe: a record
     * without a verifiable header never loads.  Yields between erases.
     */
    bool EraseSlot(uint8_t slot)
    {
        if (slot >= kNumSlots) return false;

        bool ok = true;
        for (uint8_t side = 0u; side < 2u; side++)
        {
            const uintptr_t a = SideAddr(slot, side);
            if (!ops_.erase(ops_.ctx, a, a + sector_size_))
            {
                ok = false;
                continue;
            }
            Yield();
            ops_.invalidate(ops_.ctx, a, sector_size_);
            sectors_[slot][side].valid = false;
            sectors_[slot][side].seq   = 0u;
        }
        return ok;
    }

  private:
    /* ── On-flash record header ──────────────────────────────────── */

    struct RecordHeader
    {
        uint32_t magic;
        uint16_t version;
        uint16_t reserved;
        uint32_t seq;
        uint32_t payload_len;
        uint32_t crc;
    } __attribute__((packed));
    static_assert(sizeof(RecordHeader) == 20u, "RecordHeader must be 20 bytes");

    static constexpr uint32_t kMagic   = 0x45505354u; /* 'EPST' */
    static constexpr uint16_t kVersion = 1u;

    /* ── Helpers ─────────────────────────────────────────────────── */

    void Yield() { if (yield_) yield_(yield_ctx_); }

    uint32_t  SideSize() const { return sector_size_ * sectors_per_side_; }
    uint32_t  SlotSize() const { return SideSize() * 2u; }
    uintptr_t SideAddr(uint8_t slot, uint8_t side) const
    {
        return flash_base_
             + static_cast<uintptr_t>(slot) * SlotSize()
             + static_cast<uintptr_t>(side) * SideSize();
    }

    static uint32_t Crc32(const uint8_t* data, size_t len)
    {
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0u; i < len; i++)
        {
            crc ^= static_cast<uint32_t>(data[i]);
            for (uint32_t b = 0u; b < 8u; b++)
            {
                uint32_t mask = (crc & 1u) ? 0xFFFFFFFFu : 0u;
                crc = (crc >> 1u) ^ (0xEDB88320u & mask);
            }
        }
        return ~crc;
    }

    /* ── State ───────────────────────────────────────────────────── */

    struct SectorState { uint32_t seq; bool valid; };

    FlashOps    ops_              = {};
    uintptr_t   flash_base_       = 0u;
    uint32_t    sector_size_      = 4096u;
    uint8_t     sectors_per_side_ = 5u;
    uint32_t    next_seq_         = 0u;

    PresetStoreYieldFn yield_     = nullptr;
    void*              yield_ctx_ = nullptr;

    SectorState sectors_[kNumSlots][2] = {};

    /* Scratch buffer for QSPI writes — avoids a stack allocation of
     * ~sizeof(Payload) bytes inside Save(). */
    uint8_t write_buf_[sizeof(RecordHeader) + sizeof(Payload)] = {};
};

} // namespace alchemy
