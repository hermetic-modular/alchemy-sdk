/**
 * @file alchemy/host_link/wire.h
 * @brief HostLink protocol v1 wire constants and little-endian helpers.
 *
 * Authoritative spec: docs/hostlink-protocol.md.  Frame layout (decoded):
 *
 *   [u8 proto][u8 type][u16 seq][u16 len][body…][u32 crc32]
 *
 * crc32 covers proto..body.  Frames travel COBS-encoded, 0x00-delimited.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include "alchemy/host_link/cobs.h"

namespace alchemy {
namespace hostlink {

constexpr uint8_t  kProtoVersion = 1u;

/* Max request/response body bytes, announced in HELLO.  512 is the
 * protocol default; storage-capable modules may raise it (≤ 2048) for
 * file-transfer throughput — hosts learn the value from HELLO, which
 * itself always fits the 512-byte minimum. */
#ifndef ALCHEMY_HOSTLINK_MAX_BODY
#define ALCHEMY_HOSTLINK_MAX_BODY 512
#endif
constexpr uint16_t kMaxBody = ALCHEMY_HOSTLINK_MAX_BODY;
static_assert(kMaxBody >= 512u && kMaxBody <= 2048u,
              "max_body must be 512..2048 (protocol §1)");

constexpr size_t   kHeaderSize   = 6u;
constexpr size_t   kCrcSize      = 4u;
constexpr size_t   kMaxDecoded   = kHeaderSize + kMaxBody + kCrcSize;
constexpr size_t   kMaxWire      = CobsMaxEncoded(kMaxDecoded) + 1u; /* +delim */

enum class Cmd : uint8_t
{
    Hello         = 0x01,
    GetDescriptor = 0x02,
    ListSlots     = 0x10,
    ReadSlot      = 0x11,
    BlobBegin     = 0x12,
    BlobData      = 0x13,
    BlobCommit    = 0x14,
    EraseSlot     = 0x15,
    GetLive       = 0x20,
    SaveToSlot    = 0x30,
    LoadFromSlot  = 0x31,
    Reboot        = 0x40,

    /* Filesystem block (protocol §8) — optional; modules without SD
     * storage answer UNSUPPORTED for the whole 0x50 range. */
    FsInfo        = 0x50,
    FsList        = 0x51,
    FsStat        = 0x52,
    FsOpenRead    = 0x53,
    FsRead        = 0x54,
    FsClose       = 0x55,
    FsOpenWrite   = 0x56,
    FsWrite       = 0x57,
    FsCommit      = 0x58,
    FsDelete      = 0x59,
    FsMkdir       = 0x5A,
    FsRename      = 0x5B,
};

constexpr uint8_t kRespFlag = 0x80u;
constexpr uint8_t kErrType  = 0xFFu;

enum class Status : uint8_t
{
    Ok             = 0,
    Unsupported    = 1,
    BadArgs        = 2,
    BadState       = 3,
    BadCrc         = 4,
    BadSlot        = 5,
    TooLarge       = 6,
    SchemaMismatch = 7,
    FlashFail      = 8,
    Busy           = 9,
    FrameError     = 10,

    /* Filesystem block (protocol §8). */
    FsNoCard       = 11,
    FsNoFile       = 12,
    FsExists       = 13,
    FsLocked       = 14,
    FsFull         = 15,
    FsIo           = 16,
};

/** BLOB_BEGIN target selecting the running (volatile) state. */
constexpr uint8_t kTargetLive = 0xFFu;

/* ── Filesystem block constants (protocol §8) ──────────────────────── */

/** FS_LIST terminal cookie: no more entries. */
constexpr uint32_t kFsListEnd = 0xFFFFFFFFu;

/** Path limits (§8.1). */
constexpr size_t kFsMaxPath    = 192u;
constexpr size_t kFsMaxSegment = 64u;

/** FS_LIST / FS_STAT entry flag bits. */
constexpr uint8_t kFsEntryDir      = 1u << 0;
constexpr uint8_t kFsEntryReadOnly = 1u << 1;
constexpr uint8_t kFsEntryLocked   = 1u << 2;

/** FS_OPEN_WRITE flag bits. */
constexpr uint8_t kFsWriteOverwrite = 1u << 0;
constexpr uint8_t kFsWriteMkdirs    = 1u << 1;
constexpr uint8_t kFsWriteResume    = 1u << 2;

/** FS_INFO flag bits. */
constexpr uint8_t kFsInfoFreeValid = 1u << 0;
constexpr uint8_t kFsInfoFwBusy    = 1u << 1;

/** REBOOT modes. */
constexpr uint8_t kRebootApp        = 0u;
constexpr uint8_t kRebootBootloader = 1u;

/* ── Endian helpers (portable across host/target) ──────────────────── */

inline void     WrU16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void     WrU32(uint8_t* p, uint32_t v)
{
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
inline uint16_t RdU16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t RdU32(const uint8_t* p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8)
         | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

} // namespace hostlink
} // namespace alchemy
