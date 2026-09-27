/**
 * @file framework/src/storage/firmware_picker.cpp
 * @brief FirmwarePicker — see alchemy/storage/firmware_picker.h.
 *
 * Memory: SDMMC1 reads by DMA, which cannot reach DTCM, where a
 * bootloader build's .bss lives.  Everything FatFS reads into (the FIL
 * and the chunk buffer) is in `.axi_bss`, per the placement rules in
 * alchemy/storage/sd_card.h, and cleared in Install() because the
 * section is NOLOAD.  Chunk reads are 4 KB at 4 KB offsets, so FatFS's
 * direct-sector transfers always land word-aligned (rule 3); the 8-byte
 * vector-table read goes through FatFS's own sector window and is exempt.
 */

#include "alchemy/storage/firmware_picker.h"

#include <cmath>
#include <cstring>

#include "daisy_seed.h"
#include "ff.h"

#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/storage/sd_card.h"
#include "alchemy/surface/settings.h"

#if defined(ALCHEMY_BOARD_V2)
#include "alchemy/hw/v2_calibration.h"
#endif

namespace alchemy {

namespace {

constexpr uint32_t kChunk     = 4096u;
constexpr uint32_t kEraseStep = 65536u;

/* Arming gesture thresholds, pot 0..1. */
constexpr float kArmBelow  = 0.85f;
constexpr float kFireAbove = 0.95f;

/* A gap longer than this between FILE ticks is the page being opened. */
constexpr uint32_t kReentryMs = 500u;

/* FatFS-touched: DMA-reachable, 32-byte aligned, cleared in Install(). */
alignas(32) ALCHEMY_SDMMC_BSS FIL     s_fil;
alignas(32) ALCHEMY_SDMMC_BSS uint8_t s_chunk[kChunk + 32u];

constexpr LedPanel::Rgb kFileOn  = {0x40, 0xFF, 0x60};
constexpr LedPanel::Rgb kFileOff = {0x10, 0x18, 0x10};
constexpr LedPanel::Rgb kNoCard  = {0x80, 0x00, 0x00};
constexpr LedPanel::Rgb kNoDir   = {0x80, 0x30, 0x00};
constexpr LedPanel::Rgb kArm     = {0xFF, 0x60, 0x00};
constexpr LedPanel::Rgb kErase   = {0xFF, 0xA0, 0x00};
constexpr LedPanel::Rgb kWrite   = {0x20, 0x60, 0xFF};
constexpr LedPanel::Rgb kVerify  = {0x20, 0xFF, 0x60};
constexpr LedPanel::Rgb kCompare = {0x20, 0xC0, 0xC0};
constexpr LedPanel::Rgb kSame    = {0x40, 0xFF, 0x60};
constexpr LedPanel::Rgb kDone    = {0xFF, 0xFF, 0xFF};
constexpr LedPanel::Rgb kError   = {0xFF, 0x00, 0x00};
constexpr LedPanel::Rgb kDfu     = {0xC0, 0x20, 0xFF};

FirmwarePicker* Self(const SettingsSlot& slot)
{
    return static_cast<FirmwarePicker*>(slot.custom_ctx);
}

float HourAt(const ArcGeometry& geo, int i)
{
    return std::fmod(geo.start_hour + geo.step_hours * static_cast<float>(i), 12.0f);
}

void FillRing(LedPanel& L, uint8_t pot, const ArcGeometry& geo, float frac,
              const LedPanel::Rgb& c)
{
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    const int n = static_cast<int>(std::lround(frac * static_cast<float>(geo.arc_leds)));
    for (int i = 0; i < n; i++)
        L.SetRingByHour(pot, HourAt(geo, i), L.ScaleGlobal(c));
}

/* Custom slots get no catch from Settings; track the raw pot so the
 * renders can draw the arming bar. */
void TrackPot(SettingsSlot& slot, float phys)
{
    slot.pot.stored = phys;
    slot.pot.caught = true;
}

const char* CheckName(FirmwareImageCheck c)
{
    switch (c)
    {
        case FirmwareImageCheck::TooSmall:      return "size";
        case FirmwareImageCheck::TooLarge:      return "size";
        case FirmwareImageCheck::InternalFlash: return "flash build";
        case FirmwareImageCheck::NotBootable:   return "not bootable";
        default:                                return "";
    }
}

} // namespace

/* ── Board ──────────────────────────────────────────────────────────── */

uint32_t FirmwarePicker::SlotEnd()
{
#if defined(ALCHEMY_BOARD_V2)
    /* The calibration record sits in the 4 KB sector just below the
     * preset region; the image must stop short of both. */
    return kV2CalQspiAddr;
#else
    return kPresetFlashBase;
#endif
}

bool FirmwarePicker::Busy() const
{
    return phase_ == Phase::Comparing || phase_ == Phase::Erasing
        || phase_ == Phase::Writing || phase_ == Phase::Verifying;
}

/* ── Flashing, one step per control frame ───────────────────────────── */

void FirmwarePicker::Fail(const char* why)
{
    f_close(&s_fil);
    err_   = why;
    phase_ = Phase::Error;
}

void FirmwarePicker::BeginFlash(uint32_t now_ms)
{
    /* The card went away since the listing: rescan, so FILE shows it. */
    if (!sd_->EnsureMounted(now_ms)) { phase_ = Phase::Idle; return; }

    char path[64];
    if (!catalog_.Path(sel_, path, sizeof path))
    {
        err_   = "path";
        phase_ = Phase::Error;
        return;
    }

    SdCard::BusyGuard guard(*sd_);
    std::memset(&s_fil, 0, sizeof s_fil);
    if (f_open(&s_fil, path, FA_READ) != FR_OK)
    {
        err_   = "open";
        phase_ = Phase::Error;
        return;
    }

    const uint32_t size = static_cast<uint32_t>(f_size(&s_fil));
    UINT           n    = 0;
    if (f_read(&s_fil, s_chunk, 8, &n) != FR_OK || n != 8) { Fail("read"); return; }
    uint32_t sp, pc;
    std::memcpy(&sp, s_chunk, 4);
    std::memcpy(&pc, s_chunk + 4, 4);
    const FirmwareImageCheck chk = CheckFirmwareImage(size, SlotEnd(), sp, pc);
    if (chk != FirmwareImageCheck::Ok) { Fail(CheckName(chk)); return; }
    f_lseek(&s_fil, 0);

    img_size_ = size;
    off_      = 0;
    erased_   = 0;
    progress_ = 0.0f;
    err_      = "";
    phase_    = Phase::Comparing;
}

/* Before anything is erased: is the chosen file the image already in
 * the slot?  Picking the firmware that is running should cost nothing —
 * no erase, no write, no reboot.  The slot is read through the
 * memory-mapped QSPI view (mapped for BOOT_SRAM apps since
 * DaisySeed::Init, the same view StepVerify reads).  The first
 * differing chunk hands over to the erase / write / verify path from
 * the top of the file. */
void FirmwarePicker::StepCompare()
{
    SdCard::BusyGuard guard(*sd_);
    UINT              n = 0;
    if (f_read(&s_fil, s_chunk, kChunk, &n) != FR_OK || n == 0) { Fail("read"); return; }

    const uint8_t* mm = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(kFirmwareSlotBase + off_));
    SCB_InvalidateDCache_by_Addr(const_cast<uint8_t*>(mm), static_cast<int32_t>((n + 31u) & ~31u));
    if (std::memcmp(mm, s_chunk, n) != 0)
    {
        f_lseek(&s_fil, 0);
        off_      = 0;
        progress_ = 0.0f;
        phase_    = Phase::Erasing;
        return;
    }

    off_ += n;
    progress_ = static_cast<float>(off_) / static_cast<float>(img_size_);
    if (off_ >= img_size_)
    {
        f_close(&s_fil);
        phase_ = Phase::Same;
    }
}

void FirmwarePicker::StepErase()
{
    daisy::QSPIHandle& q   = hw_->seed.qspi;
    uint32_t           end = erased_ + kEraseStep;
    if (end > img_size_) end = img_size_;
    if (q.Erase(kFirmwareSlotBase + erased_, kFirmwareSlotBase + end)
        != daisy::QSPIHandle::Result::OK)
    {
        Fail("erase");
        return;
    }
    erased_   = end;
    progress_ = static_cast<float>(erased_) / static_cast<float>(img_size_);
    if (erased_ >= img_size_)
    {
        phase_    = Phase::Writing;
        progress_ = 0.0f;
    }
}

void FirmwarePicker::StepWrite()
{
    SdCard::BusyGuard guard(*sd_);
    UINT              n = 0;
    if (f_read(&s_fil, s_chunk, kChunk, &n) != FR_OK || n == 0) { Fail("read"); return; }
    if (hw_->seed.qspi.Write(kFirmwareSlotBase + off_, n, s_chunk)
        != daisy::QSPIHandle::Result::OK)
    {
        Fail("write");
        return;
    }
    off_ += n;
    progress_ = static_cast<float>(off_) / static_cast<float>(img_size_);
    if (off_ >= img_size_)
    {
        f_lseek(&s_fil, 0);
        off_      = 0;
        progress_ = 0.0f;
        phase_    = Phase::Verifying;
    }
}

void FirmwarePicker::StepVerify()
{
    SdCard::BusyGuard guard(*sd_);
    UINT              n = 0;
    if (f_read(&s_fil, s_chunk, kChunk, &n) != FR_OK || n == 0) { Fail("read"); return; }

    /* Memory-mapped view; QSPI is back in that mode after Write().  The
     * data cache may hold stale lines for the region, so invalidate. */
    const uint8_t* mm = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(kFirmwareSlotBase + off_));
    SCB_InvalidateDCache_by_Addr(const_cast<uint8_t*>(mm), static_cast<int32_t>((n + 31u) & ~31u));
    if (std::memcmp(mm, s_chunk, n) != 0) { Fail("verify"); return; }

    off_ += n;
    progress_ = static_cast<float>(off_) / static_cast<float>(img_size_);
    if (off_ >= img_size_)
    {
        f_close(&s_fil);
        phase_ = Phase::Done;
    }
}

/* ── Settings hooks ─────────────────────────────────────────────────── */

/* Pot 0: FILE.  Ticks only while the page is visible, so the first tick
 * after a gap is the page being opened: disarm, and rescan unless a
 * flash is in flight. */
void FirmwarePicker::FileTick(SettingsSlot& slot, float phys, uint32_t t_ms)
{
    FirmwarePicker& p         = *Self(slot);
    const bool      reentered = (t_ms - p.last_tick_) > kReentryMs;
    p.last_tick_              = t_ms;

    if (reentered)
    {
        p.flash_armed_ = false;
        p.dfu_armed_   = false;
        if (!p.Busy()) p.phase_ = Phase::Idle;
    }

    const bool no_card = p.catalog_.State() == FirmwareCatalog::Status::NoCard;
    if (p.phase_ == Phase::Idle || (p.phase_ == Phase::Listing && no_card))
    {
        /* NoCard retries ride EnsureMounted()'s own backoff. */
        p.catalog_.Scan(t_ms);
        p.sel_   = 0;
        p.phase_ = Phase::Listing;
    }

    const uint8_t count = p.catalog_.Count();
    if (p.phase_ == Phase::Listing && count > 0)
    {
        int s = static_cast<int>(phys * static_cast<float>(count));
        if (s < 0) s = 0;
        if (s >= count) s = count - 1;
        p.sel_ = static_cast<uint8_t>(s);
    }
}

void FirmwarePicker::FileRender(const SettingsSlot& slot, LedPanel& L, uint8_t pot,
                                const ArcGeometry& geo, uint32_t t_ms)
{
    (void)t_ms;
    const FirmwarePicker& p = *Self(slot);
    L.ClearRing(pot);
    const int arc = geo.arc_leds;

    switch (p.catalog_.State())
    {
        case FirmwareCatalog::Status::NotScanned:
            break;
        case FirmwareCatalog::Status::NoCard:
            L.SetRingByHour(pot, HourAt(geo, arc / 2), L.ScaleGlobal(kNoCard));
            break;
        case FirmwareCatalog::Status::NoFolder:
        case FirmwareCatalog::Status::Empty:
            L.SetRingByHour(pot, HourAt(geo, arc / 2), L.ScaleGlobal(kNoDir));
            break;
        case FirmwareCatalog::Status::Listed:
        {
            const int count = p.catalog_.Count();
            for (int i = 0; i < count; i++)
            {
                const int idx = (count == 1)
                    ? arc / 2
                    : static_cast<int>(std::lround(static_cast<float>(i) * static_cast<float>(arc - 1)
                                                   / static_cast<float>(count - 1)));
                L.SetRingByHour(pot, HourAt(geo, idx),
                                L.ScaleGlobal(i == p.sel_ ? kFileOn : kFileOff));
            }
            break;
        }
    }
}

/* Pot 1: FLASH.  Dip below kArmBelow, then sweep past kFireAbove. */
void FirmwarePicker::FlashTick(SettingsSlot& slot, float phys, uint32_t t_ms)
{
    TrackPot(slot, phys);
    FirmwarePicker& p = *Self(slot);
    switch (p.phase_)
    {
        case Phase::Listing:
            if (p.catalog_.State() != FirmwareCatalog::Status::Listed) break;
            if (phys < kArmBelow) p.flash_armed_ = true;
            if (p.flash_armed_ && phys > kFireAbove)
            {
                p.flash_armed_ = false;
                p.BeginFlash(t_ms);
            }
            break;
        case Phase::Comparing: p.StepCompare(); break;
        case Phase::Erasing:   p.StepErase();   break;
        case Phase::Writing:   p.StepWrite();   break;
        case Phase::Verifying: p.StepVerify();  break;
        case Phase::Done:
            /* Hand over.  Skip the bootloader's DFU grace period: the image
             * is in the slot, boot it.  Does not return. */
            daisy::System::ResetToBootloader(daisy::System::BootloaderMode::DAISY_SKIP_TIMEOUT);
            break;
        case Phase::Same:
        case Phase::Error:
            /* Back off the pot to acknowledge; the list comes back. */
            if (phys < kArmBelow) p.phase_ = Phase::Idle;
            break;
        default:
            break;
    }
}

void FirmwarePicker::FlashRender(const SettingsSlot& slot, LedPanel& L, uint8_t pot,
                                 const ArcGeometry& geo, uint32_t t_ms)
{
    const FirmwarePicker& p = *Self(slot);
    L.ClearRing(pot);
    switch (p.phase_)
    {
        case Phase::Listing:
            /* The arming bar: how far the pot has come toward firing.  Dim
             * until the dip has been seen, so a parked pot reads as inert. */
            if (p.catalog_.State() == FirmwareCatalog::Status::Listed)
                FillRing(L, pot, geo, slot.pot.stored,
                         p.flash_armed_ ? kArm : LedPanel::Scale(kArm, 0.25f));
            break;
        case Phase::Comparing: FillRing(L, pot, geo, p.progress_, kCompare); break;
        case Phase::Same:      FillRing(L, pot, geo, 1.0f, kSame);           break;
        case Phase::Erasing:   FillRing(L, pot, geo, p.progress_, kErase);   break;
        case Phase::Writing:   FillRing(L, pot, geo, p.progress_, kWrite);   break;
        case Phase::Verifying: FillRing(L, pot, geo, p.progress_, kVerify);  break;
        case Phase::Done:      FillRing(L, pot, geo, 1.0f, kDone);           break;
        case Phase::Error:
            /* Blink, so it cannot be mistaken for a colour choice. */
            if ((t_ms / 250u) & 1u) FillRing(L, pot, geo, 1.0f, kError);
            break;
        default:
            break;
    }
}

/* Pot 2: DFU.  Same gesture; reboot into update mode and stay there. */
void FirmwarePicker::DfuTick(SettingsSlot& slot, float phys, uint32_t t_ms)
{
    (void)t_ms;
    TrackPot(slot, phys);
    FirmwarePicker& p = *Self(slot);
    if (p.Busy()) return;
    if (phys < kArmBelow) p.dfu_armed_ = true;
    if (p.dfu_armed_ && phys > kFireAbove)
        daisy::System::ResetToBootloader(daisy::System::BootloaderMode::DAISY_INFINITE_TIMEOUT);
}

void FirmwarePicker::DfuRender(const SettingsSlot& slot, LedPanel& L, uint8_t pot,
                               const ArcGeometry& geo, uint32_t t_ms)
{
    (void)t_ms;
    const FirmwarePicker& p = *Self(slot);
    L.ClearRing(pot);
    FillRing(L, pot, geo, slot.pot.stored,
             p.dfu_armed_ ? kDfu : LedPanel::Scale(kDfu, 0.25f));
}

/* ── Install ────────────────────────────────────────────────────────── */

void FirmwarePicker::Install(Settings& settings, uint8_t page, SdCard& sd, AlchemyLab& hw)
{
    sd_ = &sd;
    hw_ = &hw;
    catalog_.Init(sd);

    /* .axi_bss is NOLOAD: clear before first use. */
    std::memset(&s_fil, 0, sizeof s_fil);
    std::memset(s_chunk, 0, sizeof s_chunk);

    settings.Page(page)
        .Name("Firmware")
        .Help("Change firmware from the SD card. Put `.bin` files built for "
              "the Alchemy Lab bootloader in a folder named `alchemy` on "
              "the card. **File** picks one; **Flash** writes it, verifies "
              "it and reboots into it: turn the pot down first, then all "
              "the way up. Picking the firmware that is already running "
              "changes nothing and does not reboot. **DFU** reboots into "
              "the bootloader's update mode the same way.");

    settings.Page(page).Pot(0).Custom()
        .Tick(FileTick).Render(FileRender).Ctx(this)
        .Ident("fw.file").Name("File")
        .Help("Which file in `/alchemy` to flash. One dot per file, the "
              "chosen one bright. Red: no card. Orange: no `alchemy` folder, "
              "or no `.bin` files in it.");

    settings.Page(page).Pot(1).Custom()
        .Tick(FlashTick).Render(FlashRender).Ctx(this)
        .Ident("fw.flash").Name("Flash")
        .Help("Turn down past the arming point, then all the way up. Teal "
              "while it checks the file against the running firmware; solid "
              "green means they are the same, so nothing is written and "
              "nothing reboots (turn down to go back to the list). Otherwise "
              "orange while erasing, blue while writing, green while "
              "verifying, then white and a reboot. Blinking red: refused or "
              "failed, and nothing was booted; turn down to try again.");

    settings.Page(page).Pot(2).Custom()
        .Tick(DfuTick).Render(DfuRender).Ctx(this)
        .Ident("fw.dfu").Name("DFU")
        .Help("Turn down, then all the way up, to reboot into the "
              "bootloader's update mode and stay there for a USB flash.");
}

} // namespace alchemy
