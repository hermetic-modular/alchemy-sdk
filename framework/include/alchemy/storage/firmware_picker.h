/**
 * @file alchemy/storage/firmware_picker.h
 * @brief alchemy::FirmwarePicker — change firmware from the SD card,
 *        from a Settings page, with the module racked.
 *
 * The picker is a Settings page, not a bootloader.  It lists the `.bin`
 * files in `/alchemy` on the card (FirmwareCatalog), writes the chosen
 * one into the bootloader's app slot at 0x90040000, verifies it, and
 * reboots through the bootloader, which boots it exactly as it would a
 * DFU upload.  The bootloader itself is never touched, and neither are
 * the calibration and preset sectors above the slot.  Every firmware
 * that installs the page can hand the module to any other, so the card
 * becomes the module's library.
 *
 *   pot 0  FILE   one dot per file on the ring, the chosen one bright.
 *                 Red: no card.  Orange: no /alchemy, or no images in it.
 *   pot 1  FLASH  turn below ~85 %, then past ~95 %, to fire.  The dip is
 *                 the arming gesture: a pot parked at maximum when the
 *                 page opens cannot fire.  The ring then shows progress:
 *                 teal comparing, solid green "already the running
 *                 image" (nothing written, no reboot), orange erasing,
 *                 blue writing, green verifying, white and a reboot.
 *                 Blinking red: refused or failed, nothing booted; turn
 *                 down to go back to the list.
 *   pot 2  DFU    same gesture; reboots into the bootloader's update mode
 *                 and stays there.
 *
 * Before any erase the image is checked (CheckFirmwareImage(): size,
 * stack, reset vector; internal-flash builds refused) and compared with
 * the running image.  Work is spread over control frames — one 64 KB
 * erase or one 4 KB write/verify chunk per tick — so the LEDs keep
 * animating, HostLink keeps answering and the audio callback, which runs
 * from SRAM, keeps running.  Busy() lets the main loop hold off autosaves
 * and other QSPI traffic while it works.
 *
 * Usage (after sd.Init(), before presets.Init()):
 *
 *   static SdCard         sd;
 *   static FirmwarePicker picker;
 *   ...
 *   sd.Init();
 *   picker.Install(settings, 1, sd, hw);   // Settings page 1
 *
 * Requirements:
 *   - The firmware's linker script provides `.axi_bss` (the SDK's
 *     alchemy_stm32h750ib_sram.lds does); the picker's FIL, DIR and
 *     4 KB chunk buffer live there (alchemy/storage/sd_card.h).
 *   - One instance per firmware (the volume and the app slot are both
 *     singletons).
 *   - Tested on Alchemy Lab V2.  V1 builds, with the slot ending at the
 *     preset region, but has not been run on hardware.
 */

#pragma once

#include <cstdint>

#include "alchemy/storage/firmware_catalog.h"
#include "alchemy/storage/firmware_image.h"

namespace alchemy {

class Settings;
class SdCard;
class LedPanel;
struct ArcGeometry;
struct SettingsSlot;

#if defined(ALCHEMY_BOARD_V2)
class AlchemyLabV2;
using AlchemyLab = AlchemyLabV2;
#else
class AlchemyLabV1;
using AlchemyLab = AlchemyLabV1;
#endif

class FirmwarePicker
{
  public:
    enum class Phase : uint8_t
    {
        Idle,       ///< page not open yet, or a rescan is due
        Listing,    ///< showing the catalog (or its NoCard / NoFolder / Empty state)
        Comparing,  ///< reading the file against the running image, before any erase
        Same,       ///< it is the running image: nothing written, no reboot
        Erasing,
        Writing,
        Verifying,
        Done,       ///< verified; the next tick reboots into it
        Error,      ///< refused or failed; nothing was booted
    };

    /**
     * Declare the page: its name and help, and pots 0-2 as custom
     * controls.  Call once, after SdCard::Init() and before
     * presets.Init().  Nothing touches the card until the page is opened.
     */
    void Install(Settings& settings, uint8_t page, SdCard& sd, AlchemyLab& hw);

    /** True while an erase / write / verify is in flight. */
    bool Busy() const;

    Phase              State() const { return phase_; }
    const FirmwareCatalog& Catalog() const { return catalog_; }

    /** Why the last attempt was refused or failed ("" if it wasn't). */
    const char* LastError() const { return err_; }

    /** The first byte a firmware image must not reach on this board. */
    static uint32_t SlotEnd();

  private:
    static void FileTick(SettingsSlot& slot, float phys, uint32_t t_ms);
    static void FlashTick(SettingsSlot& slot, float phys, uint32_t t_ms);
    static void DfuTick(SettingsSlot& slot, float phys, uint32_t t_ms);
    static void FileRender(const SettingsSlot& slot, LedPanel& panel, uint8_t pot,
                           const ArcGeometry& geo, uint32_t t_ms);
    static void FlashRender(const SettingsSlot& slot, LedPanel& panel, uint8_t pot,
                            const ArcGeometry& geo, uint32_t t_ms);
    static void DfuRender(const SettingsSlot& slot, LedPanel& panel, uint8_t pot,
                          const ArcGeometry& geo, uint32_t t_ms);

    void BeginFlash(uint32_t now_ms);
    void StepCompare();
    void StepErase();
    void StepWrite();
    void StepVerify();
    void Fail(const char* why);

    SdCard*         sd_ = nullptr;
    AlchemyLab*     hw_ = nullptr;
    FirmwareCatalog catalog_;
    Phase           phase_ = Phase::Idle;
    uint8_t         sel_   = 0;

    bool     flash_armed_ = false;   ///< FLASH seen below the arming point since the page opened
    bool     dfu_armed_   = false;
    uint32_t last_tick_   = 0;       ///< to notice the page being re-entered

    uint32_t    img_size_ = 0;
    uint32_t    off_      = 0;       ///< write / verify / compare offset
    uint32_t    erased_   = 0;
    float       progress_ = 0.0f;    ///< 0..1 for the FLASH ring
    const char* err_      = "";
};

} // namespace alchemy
