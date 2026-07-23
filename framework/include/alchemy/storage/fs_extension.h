/**
 * @file alchemy/storage/fs_extension.h
 * @brief hostlink::FsExtension — the protocol §8 filesystem command
 *        block, serving the module's SD volume over HostLink.
 *
 * Attach next to the Host:
 *
 *   static alchemy::SdCard            sd;
 *   static alchemy::hostlink::FsExtension fs_ext(sd);
 *   static alchemy::hostlink::Host    host(presets, ...);
 *   ...
 *   sd.Init();                 // before audio, owns volume "0:"
 *   host.Extend(fs_ext);
 *   loop.Use(host);
 *
 * The extension advertises `"storage":{"sd":true,"fsv":1}` in the
 * descriptor root, so capable hosts light up file UI without probing.
 *
 * Execution model (per the §8 budget): every command performs at most
 * one bounded disk transfer (≤ max_body bytes of file data, or one
 * directory step), so a poll never blocks longer than a single FatFS
 * call.  Long operations — downloads, uploads, big listings — span
 * many commands, with the transfer/cursor state machines below carrying
 * context between them and Tick() expiring them after 5 s of silence.
 *
 * Buffer discipline: the frame buffers live wherever the engine lives
 * (DTCM on bootloader builds) and are NOT DMA-reachable by SDMMC1 —
 * all file bytes stage through this module's SDRAM buffer, and the
 * FIL/DIR objects are SDRAM statics per the SdCard placement rules.
 * One instance per system, like SdCard itself.
 *
 * Crash safety (§8.4): uploads write `<path>.part` plus a 16-byte
 * `<path>.partmeta` checkpoint every 1 MiB; FS_COMMIT CRC-verifies and
 * renames.  The final path only ever appears fully-formed.
 */

#pragma once

#include <cstdint>

#include "alchemy/host_link/extension.h"
#include "alchemy/host_link/wire.h"
#include "alchemy/storage/sd_card.h"

namespace alchemy {
namespace hostlink {

class FsExtension : public IHostlinkExtension
{
  public:
    explicit FsExtension(SdCard& card) : card_(card) {}

    /* ── IHostlinkExtension ─────────────────────────────────────────── */

    uint8_t FirstCmd() const override { return static_cast<uint8_t>(Cmd::FsInfo); }
    uint8_t LastCmd()  const override { return static_cast<uint8_t>(Cmd::FsRename); }

    void Handle(const ParsedFrame& f, FrameWriter& w, uint32_t now_ms) override;
    void Tick(uint32_t now_ms) override;

    const char* DescriptorRootJson() const override
    {
        return "\"storage\":{\"sd\":true,\"fsv\":1}";
    }

    /** Drop all session state (open transfer, listing cursor) WITHOUT
     *  touching the filesystem — the state a power cut leaves behind.
     *  For re-initialization paths and reboot simulation in tests;
     *  normal expiry goes through Tick(). */
    void Reset()
    {
        mode_          = XferMode::None;
        dir_open_      = false;
        pending_valid_ = false;
        dir_index_     = 0u;
        dir_emitted_   = 0u;
    }

    /** Staged-transfer inactivity timeout (§8.2). */
    static constexpr uint32_t kXferTimeoutMs = 5000u;

    /** Upload meta-checkpoint interval (§8.4). */
    static constexpr uint32_t kMetaIntervalBytes = 1024u * 1024u;

  private:
    enum class XferMode : uint8_t { None, Read, Write };

    /* Command handlers (each writes the full response body into w). */
    void OnInfo     (const ParsedFrame& f, FrameWriter& w, uint32_t now_ms);
    void OnList     (const ParsedFrame& f, FrameWriter& w, uint32_t now_ms);
    void OnStat     (const ParsedFrame& f, FrameWriter& w, uint32_t now_ms);
    void OnOpenRead (const ParsedFrame& f, FrameWriter& w, uint32_t now_ms);
    void OnRead     (const ParsedFrame& f, FrameWriter& w, uint32_t now_ms);
    void OnClose    (const ParsedFrame& f, FrameWriter& w);
    void OnOpenWrite(const ParsedFrame& f, FrameWriter& w, uint32_t now_ms);
    void OnWrite    (const ParsedFrame& f, FrameWriter& w, uint32_t now_ms);
    void OnCommit   (const ParsedFrame& f, FrameWriter& w);
    void OnDelete   (const ParsedFrame& f, FrameWriter& w, uint32_t now_ms);
    void OnMkdir    (const ParsedFrame& f, FrameWriter& w, uint32_t now_ms);
    void OnRename   (const ParsedFrame& f, FrameWriter& w, uint32_t now_ms);

    /* Transfer/cursor lifecycle. */
    void CloseTransfer(bool checkpoint_meta);
    void CloseDirCursor();
    bool WriteMeta();                    /* checkpoint received/crc      */
    bool CollidesWithTransfer(const char* path) const;

    SdCard& card_;

    /* ── Open transfer (at most one, read or write) ─────────────────── */
    XferMode mode_        = XferMode::None;
    uint32_t xfer_size_   = 0u;   /* read: file size; write: total_len  */
    uint32_t xfer_pos_    = 0u;   /* write: bytes staged                */
    uint32_t xfer_crc_    = 0u;   /* write: chained CRC32 of staged     */
    uint32_t meta_synced_ = 0u;   /* bytes at last meta checkpoint      */
    uint8_t  write_flags_ = 0u;
    uint32_t xfer_last_ms_ = 0u;

    /* ── Directory cursor (one, with one-entry lookahead) ───────────── */
    bool     dir_open_    = false;
    uint32_t dir_index_   = 0u;   /* entries CONSUMED from the DIR      */
    uint32_t dir_emitted_ = 0u;   /* entries actually sent to the host  */
    uint32_t dir_last_ms_ = 0u;
    bool     pending_valid_ = false;
    uint8_t  pending_flags_ = 0u;
    uint32_t pending_size_  = 0u;
    uint32_t pending_mtime_ = 0u;
    char     pending_name_[kFsMaxSegment + 1u] = {};
};

} // namespace hostlink
} // namespace alchemy
