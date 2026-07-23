/**
 * @file tests/host/ram_diskio.h
 * @brief RAM-backed FatFS diskio for host tests, with fault injection.
 *
 * Compiled together with the real ff.c, this gives the full FatFS stack
 * (mount, mkfs, directories, FAT chains) in-process — so the storage
 * layer (SdCard, FsExtension) is exercised end-to-end with zero
 * hardware, including card-yank and power-cut/resume scenarios:
 *
 *   - FailAfterWrites(n): the next n sector writes succeed, then every
 *     read/write fails (the card "vanishes" mid-operation).
 *   - The image persists across simulated reboots (SdCard::Init() +
 *     fresh service objects) unless Reset() is called, which is what
 *     makes .part/.partmeta resume testable.
 */

#pragma once

#include <cstdint>

namespace ramdisk {

constexpr uint32_t kSectorBytes = 512u;

/** (Re)create a blank, unformatted image of @p sectors and clear all
 *  fault state.  Call once per test scenario before f_mkfs. */
void Reset(uint32_t sectors);

/** Total sectors in the current image. */
uint32_t Sectors();

/** Direct image access (for corruption tests). */
uint8_t* Image();

/** Allow @p n more sector writes, then fail everything (reads too).
 *  Pass a negative value to clear the fault. */
void FailAfterWrites(int32_t n);

/** Immediate total failure toggle (card yanked). */
void FailAll(bool on);

/** Clear all injected faults (the "card re-seated" moment). */
void ClearFaults();

/** Sector writes performed since the last Reset(). */
uint32_t WriteCount();

/** disk_read/disk_write calls whose buffer pointer was not word-aligned
 *  since process start.  On hardware the SDMMC IDMA silently truncates
 *  such addresses (data shifts 1–3 bytes); the suite asserts zero. */
uint32_t UnalignedIo();

} // namespace ramdisk
