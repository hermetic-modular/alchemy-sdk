/**
 * @file alchemy/host_link/crc32.h
 * @brief CRC32 (reflected, poly 0xEDB88320) shared by the HostLink wire
 *        protocol, blob commits, and descriptor integrity checks.
 *
 * Bit-identical to the CRC the preset store stamps into flash records —
 * one polynomial everywhere keeps host implementations honest.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace alchemy {
namespace hostlink {

/** Incremental form: pass the previous return value as @p seed to
 *  continue a running CRC across buffers.  Initial seed is 0. */
inline uint32_t Crc32(const uint8_t* data, size_t len, uint32_t seed = 0u)
{
    uint32_t crc = ~seed;
    for (size_t i = 0u; i < len; i++)
    {
        crc ^= static_cast<uint32_t>(data[i]);
        for (uint32_t b = 0u; b < 8u; b++)
        {
            const uint32_t mask = (crc & 1u) ? 0xFFFFFFFFu : 0u;
            crc = (crc >> 1u) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

} // namespace hostlink
} // namespace alchemy
