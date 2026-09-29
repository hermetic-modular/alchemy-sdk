/**
 * @file alchemy/control/selector.h
 * @brief Equal-width value bins for discrete controls.
 */
#pragma once

#include <cstdint>

namespace alchemy {

/** Select floor(value01 * zones), clamped to [0, zones-1]. Each bin is
 *  [i/zones, (i+1)/zones), with 1 included in the final bin. Zero/one
 *  zones and NaN return 0. This is value quantization, independent of
 *  where the corresponding LED indicators are placed. */
inline uint8_t SelectorIndex(float value01, uint8_t zones)
{
    if (zones <= 1u || !(value01 > 0.0f)) return 0u;
    if (value01 >= 1.0f) return static_cast<uint8_t>(zones - 1u);
    const auto index = static_cast<uint8_t>(value01 * zones);
    return index < zones ? index : static_cast<uint8_t>(zones - 1u);
}

} // namespace alchemy
