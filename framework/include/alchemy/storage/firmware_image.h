/**
 * @file alchemy/storage/firmware_image.h
 * @brief Pure rules for firmware images on the SD card: which files are
 *        candidates, and which images are safe to write into the
 *        bootloader's app slot.
 *
 * Header-only and board-free, so the rules are host-tested
 * (tests/firmware_image_test.cpp) apart from the flash path that uses
 * them (alchemy/storage/firmware_picker.h).
 *
 * The app slot is where the Daisy bootloader expects a BOOT_SRAM image:
 * QSPI 0x90040000, the ORIGIN of QSPIFLASH in
 * cmake/linkers/alchemy_stm32h750ib_sram.lds, the address every
 * `<name>-flash` target and the web programmer write to.  Writing the
 * same bytes there from the card is indistinguishable, to the
 * bootloader, from a DFU upload.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <strings.h>

namespace alchemy {

/** First byte of the bootloader's app slot (QSPI, memory-mapped). */
constexpr uint32_t kFirmwareSlotBase = 0x90040000u;

/** Smallest file taken as an image: a vector table plus code. */
constexpr uint32_t kFirmwareMinBytes = 512u;

/** Longest file name the picker lists (FAT long names, no directory). */
constexpr size_t kFirmwareNameMax = 31u;

/** Why an image was refused, or Ok. */
enum class FirmwareImageCheck : uint8_t
{
    Ok,
    TooSmall,       ///< under kFirmwareMinBytes
    TooLarge,       ///< would reach the calibration / preset sectors
    InternalFlash,  ///< built for internal flash (reset vector 0x08xxxxxx)
    NotBootable,    ///< stack or reset vector outside what the bootloader runs
};

/**
 * The test the bootloader applies before it jumps, done before anything
 * is erased: the initial stack pointer must be in DTCM or AXI SRAM, and
 * the reset vector in AXI SRAM (a BOOT_SRAM image) or in the QSPI app
 * slot.  An image linked for internal flash is refused outright rather
 * than written into the slot, where the bootloader would refuse to boot
 * it and the module would sit in DFU.
 *
 * @param size      file size in bytes
 * @param slot_end  first byte the image must not reach (calibration +
 *                  presets start here; see FirmwareSlotEnd())
 * @param sp        word 0 of the image, the initial stack pointer
 * @param pc        word 1 of the image, the reset vector
 */
constexpr FirmwareImageCheck CheckFirmwareImage(uint32_t size, uint32_t slot_end,
                                                uint32_t sp, uint32_t pc)
{
    constexpr uint32_t kDtcmLo   = 0x20000000u, kDtcmHi   = 0x20020000u;
    constexpr uint32_t kAxiLo    = 0x24000000u, kAxiHi    = 0x24080000u;
    constexpr uint32_t kIflashLo = 0x08000000u, kIflashHi = 0x08020000u;
    constexpr uint32_t kQspiEnd  = 0x90000000u + 8u * 1024u * 1024u;

    if (size < kFirmwareMinBytes) return FirmwareImageCheck::TooSmall;
    if (size > slot_end - kFirmwareSlotBase) return FirmwareImageCheck::TooLarge;
    if (pc >= kIflashLo && pc < kIflashHi) return FirmwareImageCheck::InternalFlash;

    const bool sp_ok = (sp >= kDtcmLo && sp <= kDtcmHi) || (sp >= kAxiLo && sp <= kAxiHi);
    const bool pc_ok = (pc >= kAxiLo && pc < kAxiHi) || (pc >= kFirmwareSlotBase && pc < kQspiEnd);
    return (sp_ok && pc_ok) ? FirmwareImageCheck::Ok : FirmwareImageCheck::NotBootable;
}

/**
 * True for a file the picker offers: a `.bin` (any case) whose name fits
 * kFirmwareNameMax.  Names starting with '.' are skipped, because macOS
 * writes an AppleDouble `._name.bin` beside every file it copies to a
 * FAT card, and those are not firmware.
 */
inline bool IsFirmwareFileName(const char* name)
{
    if (!name || name[0] == '\0' || name[0] == '.') return false;
    size_t n = 0;
    while (name[n] != '\0') n++;
    if (n < 5u || n > kFirmwareNameMax) return false;
    return strcasecmp(name + n - 4u, ".bin") == 0;
}

} // namespace alchemy
