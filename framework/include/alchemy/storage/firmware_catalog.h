/**
 * @file alchemy/storage/firmware_catalog.h
 * @brief alchemy::FirmwareCatalog — the firmware images in `/alchemy`
 *        on the SD card, listed the same way every time.
 *
 * Scan() reads the folder once and keeps up to kMaxFiles names, sorted
 * case-insensitively, so the list (and a ring that draws it) is stable
 * across cards and rescans.  Files are filtered by IsFirmwareFileName()
 * (alchemy/storage/firmware_image.h).  Nothing here touches flash; the
 * picker (alchemy/storage/firmware_picker.h) builds on it.
 *
 * Control-loop thread only.  Uses the SdCard volume and its BusyGuard,
 * and places its FatFS DIR under the SD-DMA rules in
 * alchemy/storage/sd_card.h.  Host-tested on the RAM diskio
 * (tests/host/firmware_tests.cpp).
 */

#pragma once

#include <cstdint>

#include "alchemy/storage/firmware_image.h"

namespace alchemy {

class SdCard;

class FirmwareCatalog
{
  public:
    /** The folder, in FatFS volume form. */
    static constexpr const char* kDir = "0:/alchemy";

    /** Most files listed; one ring of 16 LEDs draws them. */
    static constexpr uint8_t kMaxFiles = 16u;

    enum class Status : uint8_t
    {
        NotScanned,
        NoCard,     ///< the volume would not mount
        NoFolder,   ///< mounted, but there is no /alchemy
        Empty,      ///< /alchemy holds no firmware images
        Listed,
    };

    /** Clear the catalog's FatFS objects (they live in NOLOAD AXI SRAM).
     *  Call once at boot, after SdCard::Init(). */
    void Init(SdCard& sd);

    /** Re-read the folder.  @p now_ms feeds SdCard::EnsureMounted()'s
     *  retry backoff.  Returns the resulting status. */
    Status Scan(uint32_t now_ms);

    Status      State() const { return status_; }
    uint8_t     Count() const { return count_; }
    const char* Name(uint8_t i) const { return i < count_ ? names_[i] : ""; }
    uint32_t    Size(uint8_t i) const { return i < count_ ? sizes_[i] : 0u; }

    /** Protocol path for entry @p i ("0:/alchemy/<name>") into @p out.
     *  Returns false if @p i is out of range or @p out is too small. */
    bool Path(uint8_t i, char* out, uint32_t out_len) const;

  private:
    SdCard*  sd_     = nullptr;
    Status   status_ = Status::NotScanned;
    uint8_t  count_  = 0;
    char     names_[kMaxFiles][kFirmwareNameMax + 1u] = {};
    uint32_t sizes_[kMaxFiles] = {};
};

} // namespace alchemy
