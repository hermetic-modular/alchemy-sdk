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
constexpr uint16_t kMaxBody      = 512u;
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
};

/** BLOB_BEGIN target selecting the running (volatile) state. */
constexpr uint8_t kTargetLive = 0xFFu;

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
