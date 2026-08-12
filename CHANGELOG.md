# Alchemy SDK v0.9.0 — 2026-08-14

The first versioned Alchemy SDK release, trying out a new cadence instead of merging PRs as I please.

## Parameter locks: configurable length (#11)

- `ParamLock<slots, LockLength<seconds, rateHz, storage>>` — length, rate, and
  storage policy are per-module choices; the default is 16 s @ 30 Hz (up from 4s). Guide: `docs/param-locks.md`.
- Motion stores as u16 : half the preset bytes per sample, so double the length fits the same preset budget.
- `LockStore::RamOnly` keeps a lock surface out of presets entirely if you're playing tight with the preset bundel.

## Buttons: VirtualButton runtime API + ButtonBank surface (#12)

- One declaration derives zones, labels, the tap-cycle gesture, browser chips,
  per-zone LEDs, and the preset byte. Buttons self-describe as a
  `kind:"buttons"` component; `Anchor()` renders a button beneath a named
  field. Guide: `docs/buttons.md`.
- Misc fix that snuck into this PR: the root `CMakeLists.txt` self-locates, so the SDK works under `add_subdirectory()`.

## ControlLoop: CV keeps running through Settings (#14)

- Fixed stupid issue where CV matrix was skipped when settings was open.

## Pager direct addressing + settings gestures metadata (#17)

- `Pager::GoToPage(page, phys)` goes to a target page with pot catch as expected.
- Settings components advertise gesture-owned pots (`gestures: [{page, pot, name}]`) so the web tool can show that clearly.

## Named pot position constants (#13)

- Some nice aliases for the canonical pot positions; examples updated to use them.

## Flash without touching the module

`hostlink-cli` can reboot a running module straight into the system bootloader over the same USB connection the web editor uses, so no need to power cycle the rack. Pair it with dfu-util in a Makefile target for one-command flashing:

```make
.PHONY: program-live
program-live: all
	node $(ALCHEMY_DIR)/tools/hostlink-cli/hostlink.mjs reboot bootloader
	dfu-util -w -a 0 -s $(FLASH_ADDRESS):leave -D $(BUILD_DIR)/$(TARGET_BIN) -d ,0483:$(USBPID)
```

So: you can now use `make program-live` to reboot the module over USB and flash new firmware. Just add this to your makefile. This has already been added to the template repo.

## New documentation

- Incredible new docs are now at https://hermeticmodular.com/docs

## Upgrading from unversioned main

- Presets survive **except recorded lock motion**, which resets once (`'PLK0'` → `'PLK1'`).
- Lock configs whose arena exceeds 16 KiB stop compiling until placed deliberately — see `docs/param-locks.md`. This probably won't impact you.
- Back compat looks good: older hosts ignore the new descriptor keys; older firmware keeps working against the updated web editor.
