/**
 * @file alchemy/host_link/cobs.h
 * @brief Consistent Overhead Byte Stuffing — HostLink frame boundaries.
 *
 * Encoded frames contain no 0x00 bytes; a single 0x00 after each frame
 * delimits it.  A receiver that joins mid-stream resynchronises at the
 * next delimiter for free.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace alchemy {
namespace hostlink {

/** Worst-case encoded size for @p n payload bytes (excludes delimiter). */
constexpr size_t CobsMaxEncoded(size_t n) { return n + n / 254u + 1u; }

/**
 * Encode @p n bytes from @p in into @p out (no delimiter appended).
 * @p out must hold CobsMaxEncoded(n) bytes.  Returns encoded length.
 */
inline size_t CobsEncode(const uint8_t* in, size_t n, uint8_t* out)
{
    size_t  out_len = 0u;
    size_t  code_at = out_len++;   /* index of the pending code byte */
    uint8_t code    = 1u;

    for (size_t i = 0u; i < n; i++)
    {
        if (in[i] == 0u)
        {
            out[code_at] = code;
            code_at      = out_len++;
            code         = 1u;
        }
        else
        {
            out[out_len++] = in[i];
            code++;
            if (code == 0xFFu)
            {
                out[code_at] = code;
                code_at      = out_len++;
                code         = 1u;
            }
        }
    }
    out[code_at] = code;
    return out_len;
}

/**
 * Decode @p n encoded bytes (one whole frame, delimiter excluded) into
 * @p out, which must hold at least @p n bytes.  Returns decoded length,
 * or 0 on malformed input (embedded zero, truncated group, empty input).
 */
inline size_t CobsDecode(const uint8_t* in, size_t n, uint8_t* out)
{
    if (n == 0u) return 0u;

    size_t out_len = 0u;
    size_t i       = 0u;
    while (i < n)
    {
        const uint8_t code = in[i++];
        if (code == 0u) return 0u;                 /* zeros never appear */
        const size_t run = static_cast<size_t>(code) - 1u;
        if (i + run > n) return 0u;                /* truncated group    */
        for (size_t k = 0u; k < run; k++)
        {
            const uint8_t b = in[i++];
            if (b == 0u) return 0u;
            out[out_len++] = b;
        }
        if (code != 0xFFu && i < n)
            out[out_len++] = 0u;                   /* implicit zero      */
    }
    return out_len;
}

} // namespace hostlink
} // namespace alchemy
