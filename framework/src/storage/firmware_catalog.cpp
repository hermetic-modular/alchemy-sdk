/**
 * @file framework/src/storage/firmware_catalog.cpp
 * @brief FirmwareCatalog — see alchemy/storage/firmware_catalog.h.
 */

#include "alchemy/storage/firmware_catalog.h"

#include <cstdio>
#include <cstring>
#include <strings.h>

#include "ff.h"

#include "alchemy/storage/sd_card.h"

namespace alchemy {

namespace {

/* FatFS-touched: DMA-reachable, 32-byte aligned, cleared in Init()
 * (sd_card.h placement rules 1 and 2; FatFS reads directory entries
 * through its own sector window, so rule 3 does not arise). */
alignas(32) ALCHEMY_SDMMC_BSS DIR     s_dir;
alignas(32) ALCHEMY_SDMMC_BSS FILINFO s_fno;

} // namespace

void FirmwareCatalog::Init(SdCard& sd)
{
    sd_     = &sd;
    status_ = Status::NotScanned;
    count_  = 0;
    std::memset(&s_dir, 0, sizeof s_dir);
    std::memset(&s_fno, 0, sizeof s_fno);
}

FirmwareCatalog::Status FirmwareCatalog::Scan(uint32_t now_ms)
{
    count_ = 0;
    if (!sd_ || !sd_->EnsureMounted(now_ms)) return status_ = Status::NoCard;

    SdCard::BusyGuard guard(*sd_);
    std::memset(&s_dir, 0, sizeof s_dir);
    if (f_opendir(&s_dir, kDir) != FR_OK) return status_ = Status::NoFolder;

    for (;;)
    {
        std::memset(&s_fno, 0, sizeof s_fno);
        if (f_readdir(&s_dir, &s_fno) != FR_OK || s_fno.fname[0] == '\0') break;
        if (s_fno.fattrib & (AM_DIR | AM_HID)) continue;
        if (!IsFirmwareFileName(s_fno.fname)) continue;
        if (count_ >= kMaxFiles) break;
        std::strcpy(names_[count_], s_fno.fname);
        sizes_[count_] = static_cast<uint32_t>(s_fno.fsize);
        count_++;
    }
    f_closedir(&s_dir);

    /* Case-insensitive insertion sort: at most 16 entries. */
    for (uint8_t i = 1; i < count_; i++)
        for (uint8_t j = i; j > 0 && strcasecmp(names_[j - 1], names_[j]) > 0; j--)
        {
            char     tn[kFirmwareNameMax + 1u];
            uint32_t ts = sizes_[j];
            std::memcpy(tn, names_[j], sizeof tn);
            std::memcpy(names_[j], names_[j - 1], sizeof tn);
            std::memcpy(names_[j - 1], tn, sizeof tn);
            sizes_[j]     = sizes_[j - 1];
            sizes_[j - 1] = ts;
        }

    return status_ = count_ ? Status::Listed : Status::Empty;
}

bool FirmwareCatalog::Path(uint8_t i, char* out, uint32_t out_len) const
{
    if (i >= count_ || !out) return false;
    const int n = std::snprintf(out, out_len, "%s/%s", kDir, names_[i]);
    return n > 0 && static_cast<uint32_t>(n) < out_len;
}

} // namespace alchemy
