# Firmware picker: change firmware from the SD card

`FirmwarePicker` is a Settings page that lists the firmware images on
the SD card and flashes the one you pick, with the module racked and no
computer attached. Every firmware that installs it can hand the module
to any other, so the card becomes the module's library.

```cpp
#include "alchemy/storage/firmware_picker.h"
#include "alchemy/storage/sd_card.h"

static SdCard         sd;
static FirmwarePicker picker;

int main()
{
    hw.Init();
    ...
    settings.UseBrightness();
    settings.UsePresets(presets);

    sd.Init();
    picker.Install(settings, 1, sd, hw);   // Settings page 2

    presets.Manage(pager);
    presets.Manage(settings);
    presets.Init();
    ...
}
```

Link `alchemy::firmware_picker`. `examples/stereo_eq` opts in this way.

## What it is, and what it is not

It is not a second bootloader. The Daisy bootloader in internal flash is
never touched. The picker writes the chosen `.bin` into the
bootloader's app slot in QSPI (0x90040000, the ORIGIN of `QSPIFLASH` in
`cmake/linkers/alchemy_stm32h750ib_sram.lds`), which is exactly what a
DFU upload or the web programmer writes. Then it reboots through the
bootloader, which boots the image as it would any other. A module that
has used the picker is indistinguishable from one flashed over USB.

## The card

```
/alchemy/
    stereo_eq.bin
    kick.bin
    ...
```

- FAT32, as `SdCard` expects.
- The folder is `/alchemy` at the root.
- Up to 16 files ending in `.bin` (any case) with names of up to 31
  characters, listed in case-insensitive alphabetical order, so the ring
  is stable across cards and rescans.
- Names starting with `.` are skipped: macOS writes an AppleDouble
  `._name.bin` beside every file it copies to a FAT card. Directories
  are skipped too.

Files reach the card by pulling it, or over USB through HostLink's
filesystem block (`FsExtension`) when the running firmware exposes it.

## The page

| Pot | Control | Ring |
|---|---|---|
| P1 | FILE | One dot per file, the chosen one bright. Red: no card. Orange: no `/alchemy`, or no images in it. |
| P2 | FLASH | Turn below ~85 %, then past ~95 %, to fire. Dim orange bar until the dip has been seen; bright orange once armed. |
| P3 | DFU | The same gesture: reboot into the bootloader's update mode and stay there. |

The dip is the arming gesture. A pot parked at maximum when the page
opens cannot fire, and reopening the page disarms both pots.

Once FLASH fires, its ring shows progress:

| Colour | Phase |
|---|---|
| teal | comparing the file with the running image |
| solid green | they are the same: nothing written, no reboot (turn down to go back) |
| orange | erasing |
| blue | writing |
| green | verifying |
| white | verified; rebooting into it |
| blinking red | refused or failed; nothing was booted (turn down to go back) |

`LastError()` names the reason: `size`, `flash build`, `not bootable`,
`open`, `read`, `erase`, `write` or `verify`.

## Safety

- **Checked before anything is erased.** `CheckFirmwareImage()`
  (`alchemy/storage/firmware_image.h`) applies the test the bootloader
  applies before it jumps: the initial stack pointer in DTCM or AXI
  SRAM, and the reset vector in AXI SRAM or the QSPI app slot. An image
  built for internal flash is refused rather than written into a slot
  the bootloader would then refuse to boot.
- **Never past the slot.** Images that would reach the calibration
  record and the preset region above the slot are refused: on V2 the
  slot ends at `kV2CalQspiAddr`, on V1 at `kPresetFlashBase`. Presets and
  the board's calibration survive every flash.
- **Compared first.** Picking the image that is already running reads
  it back against the slot and stops there: no erase, no write, no
  reboot.
- **Verified after writing.** The slot is read back through the
  memory-mapped QSPI view (cache invalidated first) and compared with
  the file before the reboot. A mismatch stops with blinking red.
- **Spread over frames.** One 64 KB erase or one 4 KB write / verify
  chunk per control frame, so the LEDs keep animating, HostLink keeps
  answering and the audio callback, which runs from SRAM, keeps
  running. `picker.Busy()` is true while it works; hold off autosaves
  and other QSPI writes until it clears.

A failure after the erase has begun (a card pulled mid-write, a QSPI
error) leaves the slot incomplete, as an interrupted DFU upload would.
The recovery is the same: hold B3 at power-on for update mode and flash
over USB. The picker cannot help here, because the firmware that
carries it is the one that was overwritten.

## Memory

The picker's FIL and 4 KB chunk buffer, and `FirmwareCatalog`'s DIR,
follow the SD-DMA placement rules in `alchemy/storage/sd_card.h`: they
live in `.axi_bss` in AXI SRAM, which the SDK's SRAM linker script
provides, and are cleared in `Install()`. About 6 KB in all.

## Preset slots: an open question

Presets are per module, not per firmware: every firmware on a card
shares the same 16 QSPI slots. Switching firmware with the picker makes
this visible, because each firmware's `presets.BootLoad()` reads slot 0.
If another firmware wrote slot 0, the schema gate refuses its blob and
the firmware starts from defaults.

One pattern that works: each firmware keeps its working state in a
"home" slot of its own, restores it at boot, and autosaves into it, so
switching away and back keeps its settings. That needs firmwares on the
same card not to pick the same home slot, which is a convention, not
code. Whether the SDK should reserve ranges, keep a registry, or leave
it to authors is discussed in
[#49](https://github.com/hermetic-modular/alchemy-sdk/issues/49).
Nothing in `FirmwarePicker` depends on the answer.

## Tests

- `tests/firmware_image_test.cpp`: the image and file-name rules.
- `tests/host/firmware_tests.cpp`: `FirmwareCatalog` against the real
  FatFS on the RAM disk (missing folder, filtering, sort order, the
  16-file cap, card absent and returning).

The QSPI half (erase, write, verify, reboot) needs the board and is not
host-tested.
