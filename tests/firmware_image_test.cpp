/**
 * @file firmware_image_test.cpp
 * @brief Host tests for the firmware-image rules (CheckFirmwareImage,
 *        IsFirmwareFileName) that gate what FirmwarePicker writes.
 */

#include "alchemy/storage/firmware_image.h"

#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                    \
        if (!(cond))                                                        \
        {                                                                   \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

using alchemy::CheckFirmwareImage;
using alchemy::FirmwareImageCheck;
using alchemy::IsFirmwareFileName;

/* V2: the calibration sector starts at 0x9075F000. */
constexpr uint32_t kSlotEnd = 0x9075F000u;
constexpr uint32_t kMaxSize = kSlotEnd - alchemy::kFirmwareSlotBase;

/* What the SDK's own SRAM linker script produces: stack at the top of
 * DTCM, reset handler in AXI SRAM. */
constexpr uint32_t kSp = 0x20020000u;
constexpr uint32_t kPc = 0x24000401u;

void TestBootSramImageAccepted()
{
    CHECK(CheckFirmwareImage(350000u, kSlotEnd, kSp, kPc) == FirmwareImageCheck::Ok);
    /* Stack in AXI SRAM, and a QSPI-XIP reset vector, are also bootable. */
    CHECK(CheckFirmwareImage(350000u, kSlotEnd, 0x24070000u, kPc) == FirmwareImageCheck::Ok);
    CHECK(CheckFirmwareImage(350000u, kSlotEnd, kSp, 0x90040401u) == FirmwareImageCheck::Ok);
}

void TestSizeBounds()
{
    CHECK(CheckFirmwareImage(511u, kSlotEnd, kSp, kPc) == FirmwareImageCheck::TooSmall);
    CHECK(CheckFirmwareImage(512u, kSlotEnd, kSp, kPc) == FirmwareImageCheck::Ok);
    /* Exactly up to the calibration sector is allowed, one byte more is not. */
    CHECK(CheckFirmwareImage(kMaxSize, kSlotEnd, kSp, kPc) == FirmwareImageCheck::Ok);
    CHECK(CheckFirmwareImage(kMaxSize + 1u, kSlotEnd, kSp, kPc) == FirmwareImageCheck::TooLarge);
}

void TestInternalFlashBuildRefused()
{
    /* A plain (non-bootloader) Daisy build links to internal flash. */
    CHECK(CheckFirmwareImage(100000u, kSlotEnd, kSp, 0x08000401u)
          == FirmwareImageCheck::InternalFlash);
}

void TestGarbageRefused()
{
    /* A text file renamed .bin, an all-0xFF image, a zeroed header. */
    CHECK(CheckFirmwareImage(4096u, kSlotEnd, 0x6c6c6568u, 0x6f77206fu)
          == FirmwareImageCheck::NotBootable);
    CHECK(CheckFirmwareImage(4096u, kSlotEnd, 0xFFFFFFFFu, 0xFFFFFFFFu)
          == FirmwareImageCheck::NotBootable);
    CHECK(CheckFirmwareImage(4096u, kSlotEnd, 0u, 0u) == FirmwareImageCheck::NotBootable);
    /* A reset vector past the end of the 8 MB QSPI. */
    CHECK(CheckFirmwareImage(4096u, kSlotEnd, kSp, 0x90800001u)
          == FirmwareImageCheck::NotBootable);
}

void TestFileNames()
{
    CHECK(IsFirmwareFileName("stereo_eq.bin"));
    CHECK(IsFirmwareFileName("MARK_ALCHEMY.BIN"));
    CHECK(IsFirmwareFileName("a.bin"));
    CHECK(!IsFirmwareFileName("._stereo_eq.bin"));   /* macOS AppleDouble */
    CHECK(!IsFirmwareFileName(".hidden.bin"));
    CHECK(!IsFirmwareFileName(".bin"));
    CHECK(!IsFirmwareFileName("stereo_eq.hex"));
    CHECK(!IsFirmwareFileName("stereo_eq.bin.txt"));
    CHECK(!IsFirmwareFileName(""));
    CHECK(!IsFirmwareFileName(nullptr));
    /* 31 characters fit, 32 do not. */
    CHECK(IsFirmwareFileName("abcdefghijklmnopqrstuvwxyz0.bin"));
    CHECK(!IsFirmwareFileName("abcdefghijklmnopqrstuvwxyz01.bin"));
}

} // namespace

int main()
{
    TestBootSramImageAccepted();
    TestSizeBounds();
    TestInternalFlashBuildRefused();
    TestGarbageRefused();
    TestFileNames();

    if (failures == 0) std::printf("firmware_image: all tests passed\n");
    return failures == 0 ? 0 : 1;
}
