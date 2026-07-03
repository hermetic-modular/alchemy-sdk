/**
 * @file alchemy/host_link/frame.h
 * @brief HostLink frame assembly (writer) and byte-stream parsing (parser).
 *
 * Both sides are allocation-free and bounded: the parser accumulates at
 * most one wire frame and drops oversize garbage until the next delimiter.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include "alchemy/host_link/cobs.h"
#include "alchemy/host_link/crc32.h"
#include "alchemy/host_link/wire.h"

namespace alchemy {
namespace hostlink {

/* ── FrameWriter ────────────────────────────────────────────────────── */

/**
 * Builds one decoded frame (header + body + CRC) in a caller buffer,
 * then COBS-encodes it (plus delimiter) into a wire buffer.
 *
 *   uint8_t dec[kMaxDecoded], wire[kMaxWire];
 *   FrameWriter w(dec);
 *   w.Begin(type, seq);
 *   w.U8(status); w.U32(x); w.Bytes(p, n);
 *   size_t wire_len = w.Encode(wire);
 */
class FrameWriter
{
  public:
    explicit FrameWriter(uint8_t* decoded_buf) : buf_(decoded_buf) {}

    void Begin(uint8_t type, uint16_t seq)
    {
        buf_[0] = kProtoVersion;
        buf_[1] = type;
        WrU16(buf_ + 2, seq);
        /* len patched in Encode() */
        len_      = 0u;
        overflow_ = false;
    }

    void U8(uint8_t v)   { if (Fits(1u)) buf_[kHeaderSize + len_++] = v; }
    void U16(uint16_t v) { if (Fits(2u)) { WrU16(buf_ + kHeaderSize + len_, v); len_ += 2u; } }
    void U32(uint32_t v) { if (Fits(4u)) { WrU32(buf_ + kHeaderSize + len_, v); len_ += 4u; } }

    void Bytes(const uint8_t* p, size_t n)
    {
        if (!Fits(n)) return;
        for (size_t i = 0u; i < n; i++) buf_[kHeaderSize + len_ + i] = p[i];
        len_ += static_cast<uint16_t>(n);
    }

    /** Length-prefixed ASCII string (u8 len + bytes, truncated at 255). */
    void Str(const char* s)
    {
        size_t n = 0u;
        while (s && s[n] && n < 255u) n++;
        if (!Fits(1u + n)) return;
        buf_[kHeaderSize + len_++] = static_cast<uint8_t>(n);
        for (size_t i = 0u; i < n; i++)
            buf_[kHeaderSize + len_ + i] = static_cast<uint8_t>(s[i]);
        len_ += static_cast<uint16_t>(n);
    }

    bool     Overflowed() const { return overflow_; }
    uint16_t BodyLen()    const { return len_; }

    /** Patch len, stamp CRC, COBS-encode into @p wire (kMaxWire bytes),
     *  append the 0x00 delimiter.  Returns wire length (0 on overflow). */
    size_t Encode(uint8_t* wire)
    {
        if (overflow_) return 0u;
        WrU16(buf_ + 4, len_);
        const size_t   crc_at = kHeaderSize + len_;
        const uint32_t crc    = Crc32(buf_, crc_at);
        WrU32(buf_ + crc_at, crc);

        const size_t enc = CobsEncode(buf_, crc_at + kCrcSize, wire);
        wire[enc] = 0u;
        return enc + 1u;
    }

  private:
    bool Fits(size_t n)
    {
        if (len_ + n > kMaxBody) { overflow_ = true; return false; }
        return true;
    }

    uint8_t* buf_;
    uint16_t len_      = 0u;
    bool     overflow_ = false;
};

/* ── FrameParser ────────────────────────────────────────────────────── */

/** One parsed frame.  @p body points into the parser's internal buffer —
 *  valid until the next Push() that completes a frame. */
struct ParsedFrame
{
    uint8_t        proto;
    uint8_t        type;
    uint16_t       seq;
    uint16_t       len;
    const uint8_t* body;
    bool           ok;      /* header sane + CRC verified */
};

class FrameParser
{
  public:
    /**
     * Feed one received byte.  Returns true when a delimiter completed a
     * frame; @p out is then populated.  Undecodable or runt chunks are
     * dropped silently (resync); decodable chunks with bad CRC/header
     * are surfaced with ok=false so the caller may NACK.
     */
    bool Push(uint8_t byte, ParsedFrame& out)
    {
        if (byte != 0u)
        {
            if (acc_len_ < sizeof(acc_)) acc_[acc_len_++] = byte;
            else                         overflow_ = true;
            return false;
        }

        /* Delimiter: decode what we accumulated. */
        const size_t n        = acc_len_;
        const bool   overflow = overflow_;
        acc_len_  = 0u;
        overflow_ = false;

        if (n == 0u || overflow) return false;      /* idle delim / garbage */

        const size_t dec = CobsDecode(acc_, n, dec_);
        if (dec == 0u) return false;                 /* malformed COBS */
        if (dec < kHeaderSize + kCrcSize) return false;  /* runt */

        out.proto = dec_[0];
        out.type  = dec_[1];
        out.seq   = RdU16(dec_ + 2);
        out.len   = RdU16(dec_ + 4);
        out.body  = dec_ + kHeaderSize;

        const size_t   body_len = dec - kHeaderSize - kCrcSize;
        const uint32_t crc      = RdU32(dec_ + dec - kCrcSize);
        out.ok = (out.proto == kProtoVersion)
              && (out.len == body_len)
              && (out.len <= kMaxBody)
              && (Crc32(dec_, dec - kCrcSize) == crc);
        return true;
    }

  private:
    uint8_t acc_[kMaxWire]    = {};
    size_t  acc_len_          = 0u;
    bool    overflow_         = false;
    uint8_t dec_[kMaxDecoded + kCrcSize] = {};
};

} // namespace hostlink
} // namespace alchemy
