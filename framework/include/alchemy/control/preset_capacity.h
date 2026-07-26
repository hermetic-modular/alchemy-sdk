/**
 * @file alchemy/control/preset_capacity.h
 * @brief How many payload bytes fit in one preset slot.
 *
 * Split out of surface/presets.h so components can budget against it
 * without dragging in `daisy_seed.h`: ParamLock static_asserts its
 * configuration against `kPresetBlobCapacity` at the declaration site,
 * and it must stay includable from board-agnostic headers.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace alchemy {

/** Erase granule of the preset region (IS25LP064A sector). */
constexpr uint32_t kPresetCapacitySectorSize = 4096u;

/** Sectors per ping-pong side of one preset slot. */
constexpr uint8_t kPresetCapacitySectorsPerSide = 5u;

/**
 * Bytes available per slot for the concatenated managed-component payload.
 *
 * Sector space minus the on-flash record header (20 B), our own blob
 * preamble (8 B), and a 4-byte safety margin.
 *
 * This ceiling is hard.  The preset region occupies the top 640 KiB of
 * the QSPI chip (16 slots × 2 sides × 5 sectors × 4 KiB, ending at the
 * last byte of the device), so it cannot be grown without either taking
 * space from firmware or trading away preset slots.
 */
constexpr size_t kPresetBlobCapacity =
    static_cast<size_t>(kPresetCapacitySectorSize) * kPresetCapacitySectorsPerSide
    - 32u;

} // namespace alchemy
