/**
 * @file host_link/host_link.cpp
 * @brief HostLink protocol engine implementation.
 */

#include "alchemy/host_link/host_link.h"

#include <cstring>
#include "alchemy/host_link/crc32.h"

namespace alchemy {
namespace hostlink {

HostLink::HostLink(IHostTransport& transport, Presets& presets,
                   const Info& info,
                   uint8_t* staging, uint8_t* snapshot, size_t buf_cap)
    : transport_(transport), presets_(presets), info_(info),
      staging_(staging), buf_cap_(buf_cap), snapshot_(snapshot)
{
}

/** Largest payload this link accepts/serves in one blob. */
static inline uint32_t CapOf(size_t buf_cap)
{
    return (buf_cap < kPresetBlobCapacity)
               ? static_cast<uint32_t>(buf_cap)
               : static_cast<uint32_t>(kPresetBlobCapacity);
}

void HostLink::SetDescriptor(const void* json, uint32_t len)
{
    desc_     = static_cast<const uint8_t*>(json);
    desc_len_ = (json != nullptr) ? len : 0u;
    desc_crc_ = (desc_len_ > 0u) ? Crc32(desc_, desc_len_) : 0u;
}

void HostLink::SetUid(const uint8_t uid[12])
{
    std::memcpy(uid_, uid, 12u);
}

void HostLink::SetRebootHandler(void (*fn)(uint8_t, void*), void* ctx)
{
    reboot_fn_  = fn;
    reboot_ctx_ = ctx;
}

void HostLink::Poll(uint32_t now_ms)
{
    transport_.Pump();

    /* Deferred reboot: fire once the ACK has had time to flush. */
    if (reboot_pending_
        && static_cast<int32_t>(now_ms - reboot_at_ms_) >= 0)
    {
        reboot_pending_ = false;
        if (reboot_fn_) reboot_fn_(reboot_mode_, reboot_ctx_);
    }

    /* Abort a stalled staging transaction so a crashed host can't wedge
     * future writes. */
    if (stage_open_ && (now_ms - stage_last_ms_) > kStageTimeoutMs)
        AbortStage();

    /* Rate-limit command *execution* per poll, not byte *consumption*:
     * every drained byte is always fed to the parser (so nothing already
     * removed from the transport ring is ever dropped), but once
     * kMaxFramesPerPoll commands have run this frame we stop draining and
     * resume next frame — partially-accumulated bytes stay safely in the
     * parser across polls. */
    uint8_t handled = 0u;
    while (handled < kMaxFramesPerPoll)
    {
        const size_t n = transport_.Read(rx_buf_, sizeof(rx_buf_));
        if (n == 0u) break;

        for (size_t i = 0u; i < n; i++)
        {
            ParsedFrame f;
            if (!parser_.Push(rx_buf_[i], f)) continue;

            if (!f.ok)
            {
                SendError(f.seq, Status::FrameError);
            }
            else
            {
                Handle(f, now_ms);
                ++handled;
            }
        }
    }
}

void HostLink::Handle(const ParsedFrame& f, uint32_t now_ms)
{
    switch (static_cast<Cmd>(f.type))
    {
        case Cmd::Hello:         OnHello(f);              break;
        case Cmd::GetDescriptor: OnGetDescriptor(f);      break;
        case Cmd::ListSlots:     OnListSlots(f);          break;
        case Cmd::ReadSlot:      OnReadSlot(f);           break;
        case Cmd::BlobBegin:     OnBlobBegin(f, now_ms);  break;
        case Cmd::BlobData:      OnBlobData(f, now_ms);   break;
        case Cmd::BlobCommit:    OnBlobCommit(f);         break;
        case Cmd::EraseSlot:     OnEraseSlot(f);          break;
        case Cmd::GetLive:       OnGetLive(f);            break;
        case Cmd::SaveToSlot:    OnSaveToSlot(f);         break;
        case Cmd::LoadFromSlot:  OnLoadFromSlot(f);       break;
        case Cmd::Reboot:        OnReboot(f, now_ms);     break;
        default:
            RespondStatus(f, Status::Unsupported);
            break;
    }
}

/* ── Command handlers ───────────────────────────────────────────────── */

void HostLink::OnHello(const ParsedFrame& f)
{
    FrameWriter w(dec_);
    w.Begin(f.type | kRespFlag, f.seq);
    w.U8(static_cast<uint8_t>(Status::Ok));
    w.U8(kProtoVersion);
    w.U8(info_.board);
    w.U8(Presets::kNumSlots);
    w.U8(info_.boot_slot);
    w.Bytes(uid_, 12u);
    w.U32(presets_.LiveSchemaHash());
    w.U32(CapOf(buf_cap_));
    w.U32(desc_len_);
    w.U32(desc_crc_);
    w.U16(kMaxBody);
    w.U16(static_cast<uint16_t>(presets_.LiveSize()));
    w.Str(info_.module_id);
    w.Str(info_.module_name);
    w.Str(info_.fw_version);
    w.Str(info_.fw_git);
    w.Str(info_.sdk_version);
    Send(w);
}

void HostLink::OnGetDescriptor(const ParsedFrame& f)
{
    if (f.len != 6u) { RespondStatus(f, Status::BadArgs); return; }

    const uint32_t offset  = RdU32(f.body);
    uint16_t       max_len = RdU16(f.body + 4);

    /* Response header: status(1) + offset(4) + n(2) = 7. */
    constexpr uint16_t kChunkMax = kMaxBody - 7u;
    if (max_len > kChunkMax) max_len = kChunkMax;

    uint16_t n = 0u;
    if (offset < desc_len_)
    {
        const uint32_t remain = desc_len_ - offset;
        n = (remain > max_len) ? max_len : static_cast<uint16_t>(remain);
    }

    FrameWriter w(dec_);
    w.Begin(f.type | kRespFlag, f.seq);
    w.U8(static_cast<uint8_t>(Status::Ok));
    w.U32(offset);
    w.U16(n);
    if (n > 0u) w.Bytes(desc_ + offset, n);
    Send(w);
}

void HostLink::OnListSlots(const ParsedFrame& f)
{
    FrameWriter w(dec_);
    w.Begin(f.type | kRespFlag, f.seq);
    w.U8(static_cast<uint8_t>(Status::Ok));
    w.U8(Presets::kNumSlots);
    for (uint8_t s = 0u; s < Presets::kNumSlots; s++)
    {
        const Presets::SlotInfo info = presets_.InfoFor(s);
        uint8_t flags = 0u;
        if (info.valid)        flags |= 0x01u;
        if (info.schema_match) flags |= 0x02u;
        w.U8(flags);
        w.U32(info.seq);
        w.U16(info.length);
        w.U32(info.schema_hash);
    }
    Send(w);
}

void HostLink::OnReadSlot(const ParsedFrame& f)
{
    if (f.len != 7u) { RespondStatus(f, Status::BadArgs); return; }

    const uint8_t  slot    = f.body[0];
    const uint32_t offset  = RdU32(f.body + 1);
    uint16_t       max_len = RdU16(f.body + 5);

    if (slot >= Presets::kNumSlots) { RespondStatus(f, Status::BadSlot); return; }

    /* Response header: status(1)+slot(1)+offset(4)+n(2)+total(2)+schema(4) = 14. */
    constexpr uint16_t kChunkMax = kMaxBody - 14u;
    if (max_len > kChunkMax) max_len = kChunkMax;

    uint32_t schema = 0u;
    uint16_t total  = 0u;
    uint8_t  chunk[kChunkMax];
    const int32_t n =
        presets_.ReadSlotBytes(slot, offset, chunk, max_len, &schema, &total);
    if (n < 0) { RespondStatus(f, Status::BadSlot); return; }

    FrameWriter w(dec_);
    w.Begin(f.type | kRespFlag, f.seq);
    w.U8(static_cast<uint8_t>(Status::Ok));
    w.U8(slot);
    w.U32(offset);
    w.U16(static_cast<uint16_t>(n));
    w.U16(total);
    w.U32(schema);
    if (n > 0) w.Bytes(chunk, static_cast<size_t>(n));
    Send(w);
}

void HostLink::OnBlobBegin(const ParsedFrame& f, uint32_t now_ms)
{
    if (f.len != 9u) { RespondStatus(f, Status::BadArgs); return; }

    const uint8_t  target = f.body[0];
    const uint32_t total  = RdU32(f.body + 1);
    const uint32_t schema = RdU32(f.body + 5);

    /* A new BEGIN implicitly abandons any prior transaction. */
    AbortStage();

    if (target != kTargetLive && target >= Presets::kNumSlots)
    {
        RespondStatus(f, Status::BadSlot);
        return;
    }
    if (total > CapOf(buf_cap_))
    {
        RespondStatus(f, Status::TooLarge);
        return;
    }
    /* Strict: only blobs the running firmware itself could have produced
     * are accepted; migration is the host's job (see protocol §6). */
    if (schema != presets_.LiveSchemaHash()
        || total != presets_.LiveSize())
    {
        RespondStatus(f, Status::SchemaMismatch);
        return;
    }

    stage_open_    = true;
    stage_target_  = target;
    stage_total_   = total;
    stage_recv_    = 0u;
    stage_schema_  = schema;
    stage_last_ms_ = now_ms;
    RespondStatus(f, Status::Ok);
}

void HostLink::OnBlobData(const ParsedFrame& f, uint32_t now_ms)
{
    if (f.len < 4u) { RespondStatus(f, Status::BadArgs); return; }
    if (!stage_open_) { RespondStatus(f, Status::BadState); return; }

    const uint32_t offset = RdU32(f.body);
    const uint32_t n      = f.len - 4u;

    if (offset != stage_recv_)              /* strictly in-order */
    {
        AbortStage();
        RespondStatus(f, Status::BadArgs);
        return;
    }
    if (stage_recv_ + n > stage_total_)
    {
        AbortStage();
        RespondStatus(f, Status::TooLarge);
        return;
    }

    std::memcpy(staging_ + stage_recv_, f.body + 4, n);
    stage_recv_    += n;
    stage_last_ms_  = now_ms;

    FrameWriter w(dec_);
    w.Begin(f.type | kRespFlag, f.seq);
    w.U8(static_cast<uint8_t>(Status::Ok));
    w.U32(stage_recv_);
    Send(w);
}

void HostLink::OnBlobCommit(const ParsedFrame& f)
{
    if (f.len != 4u) { RespondStatus(f, Status::BadArgs); return; }
    if (!stage_open_ || stage_recv_ != stage_total_)
    {
        AbortStage();
        RespondStatus(f, Status::BadState);
        return;
    }

    const uint32_t want = RdU32(f.body);
    const uint32_t got  = Crc32(staging_, stage_total_);
    if (want != got)
    {
        AbortStage();
        RespondStatus(f, Status::BadCrc);
        return;
    }

    Status result = Status::Ok;
    if (stage_target_ == kTargetLive)
    {
        /* The on-device load path minus flash: components re-arm their
         * pot catches on the next frame, exactly like a panel load. */
        if (!presets_.DeserializeLive(staging_, stage_total_))
            result = Status::BadArgs;
    }
    else
    {
        /* Same wear-levelled, yield-serviced path as a panel save.  This
         * blocks for the flash duration; the response goes out after. */
        if (!presets_.WriteSlotRaw(stage_target_, stage_schema_,
                                   staging_,
                                   static_cast<uint16_t>(stage_total_)))
            result = Status::FlashFail;
    }
    AbortStage();
    RespondStatus(f, result);
}

void HostLink::OnEraseSlot(const ParsedFrame& f)
{
    if (f.len != 1u) { RespondStatus(f, Status::BadArgs); return; }
    const uint8_t slot = f.body[0];
    if (slot >= Presets::kNumSlots) { RespondStatus(f, Status::BadSlot); return; }

    RespondStatus(f, presets_.EraseSlot(slot) ? Status::Ok
                                              : Status::FlashFail);
}

void HostLink::OnGetLive(const ParsedFrame& f)
{
    if (f.len != 6u) { RespondStatus(f, Status::BadArgs); return; }

    const uint32_t offset  = RdU32(f.body);
    uint16_t       max_len = RdU16(f.body + 4);

    if (offset == 0u)
    {
        /* Fresh snapshot so multi-chunk reads stay self-consistent. */
        const size_t n = presets_.SerializeLive(snapshot_, buf_cap_);
        snap_len_   = static_cast<uint16_t>(n);
        snap_valid_ = (n > 0u);
    }
    if (!snap_valid_) { RespondStatus(f, Status::BadState); return; }

    constexpr uint16_t kChunkMax = kMaxBody - 14u;
    if (max_len > kChunkMax) max_len = kChunkMax;

    uint16_t n = 0u;
    if (offset < snap_len_)
    {
        const uint32_t remain = static_cast<uint32_t>(snap_len_) - offset;
        n = (remain > max_len) ? max_len : static_cast<uint16_t>(remain);
    }

    FrameWriter w(dec_);
    w.Begin(f.type | kRespFlag, f.seq);
    w.U8(static_cast<uint8_t>(Status::Ok));
    w.U8(kTargetLive);
    w.U32(offset);
    w.U16(n);
    w.U16(snap_len_);
    w.U32(presets_.LiveSchemaHash());
    if (n > 0u) w.Bytes(snapshot_ + offset, n);
    Send(w);
}

void HostLink::OnSaveToSlot(const ParsedFrame& f)
{
    if (f.len != 1u) { RespondStatus(f, Status::BadArgs); return; }
    const uint8_t slot = f.body[0];
    if (slot >= Presets::kNumSlots) { RespondStatus(f, Status::BadSlot); return; }

    RespondStatus(f, presets_.Save(slot) ? Status::Ok : Status::FlashFail);
}

void HostLink::OnLoadFromSlot(const ParsedFrame& f)
{
    if (f.len != 1u) { RespondStatus(f, Status::BadArgs); return; }
    const uint8_t slot = f.body[0];
    if (slot >= Presets::kNumSlots) { RespondStatus(f, Status::BadSlot); return; }

    RespondStatus(f, (presets_.HasValid(slot) && presets_.Load(slot))
                         ? Status::Ok
                         : Status::BadSlot);
}

void HostLink::OnReboot(const ParsedFrame& f, uint32_t now_ms)
{
    if (f.len != 1u) { RespondStatus(f, Status::BadArgs); return; }
    const uint8_t mode = f.body[0];
    if (mode > kRebootBootloader) { RespondStatus(f, Status::BadArgs); return; }
    if (!reboot_fn_) { RespondStatus(f, Status::Unsupported); return; }

    RespondStatus(f, Status::Ok);
    reboot_pending_ = true;
    reboot_mode_    = mode;
    reboot_at_ms_   = now_ms + kRebootDelayMs;
}

/* ── Response plumbing ──────────────────────────────────────────────── */

void HostLink::RespondStatus(const ParsedFrame& f, Status s)
{
    FrameWriter w(dec_);
    w.Begin(f.type | kRespFlag, f.seq);
    w.U8(static_cast<uint8_t>(s));
    Send(w);
}

void HostLink::SendError(uint16_t seq, Status s)
{
    FrameWriter w(dec_);
    w.Begin(kErrType, seq);
    w.U8(static_cast<uint8_t>(s));
    Send(w);
}

void HostLink::Send(FrameWriter& w)
{
    const size_t n = w.Encode(wire_);
    if (n == 0u) return;                       /* response overflow — drop */
    if (transport_.WriteSpace() < n) return;   /* queue full — host times out */
    transport_.Write(wire_, n);
}

} // namespace hostlink
} // namespace alchemy
