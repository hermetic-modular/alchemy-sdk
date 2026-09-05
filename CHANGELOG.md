# Alchemy SDK v0.11.0 | 2026-09-04

## Parameter locks v2 (#29)

- Added clocked mode. `locks.UseClock(clock_)` supplies the clock;
  `settings.UseLocks(locks)` adds a persisted Free/Clocked selector on P5.
  In Clocked mode a finished recording snaps its loop boundary to the
  nearest musical division and restarts against the clock through tempo
  changes. Straight and triplet divisions by default; `SnapGrid()` adds
  dotted.
- Slot header `'PLK1'` to `'PLK2'`, 5 bytes to 7. Locks saved under the
  old header load as empty.
- Arm threshold lowered from 3% to 0.5%. Clear threshold stays 3%.
  `record_base` is now the pre-nudge origin. Added `ArmThreshold()` and
  `ClearThreshold()`.
- Added `ExitMode(LockExit::Return)`: on release the value returns to the
  recorded origin and pot catch re-arms. `LockExit::Latch` is the surface
  default and matches v0.10.0. `UseLocks` also adds a persisted
  Return/Latch selector on P6, defaulting to Return. Default settings
  stack: P1 brightness, P2 free, P3-P4 presets, P5-P6 locks.
- Added `RecordStyle()` and `PlayStyle()` (Solid, Blink, Breathe,
  LoopPulse, Sweep; red default), `ClearSlot()`, `Freeze()`, `PlayPhase()`.
- Fixed: a trigger release landing under the Settings gate was dropped, so
  `OnButtonUp` never ran and the manager kept the hold.
- Added `ParamLock::TakeCleanRelease()`: true once per trigger release
  whose hold armed or cleared nothing. A release under the Settings gate
  still reaches the manager but never counts as a tap.
- Behavior documented in `docs/param-locks.md`.

## Pager navigation bindings (#30)

- Added `Pager(num_pages, num_pots)` with `.Cycle(btn, pages...)`,
  `.Shift(btn, page)`, `.Latch(btn, a, b)` and `.From(pages...)`. The
  `Pager(b1, n, m)` constructor is unchanged and equivalent to `.Cycle(b1)`
  over every page. Serialized image and `'PAG0'` schema unchanged.
- Shift layers engage on the press edge. Pot catch re-arms in both
  directions and runs only on the active page.
- Added `Pager::HoldUsed()`. A hold that edited no parameter does not claim
  its release; a hold that edited one does.
- Latch pairs occupy one cycle slot and remember the selected member.
- Added arbitration: presses under or spanning the Settings gate are
  poisoned, co-pressing two navigation buttons aborts layers, one layer
  runs at a time, and `GoToPage` cancels an active layer.
- Added `KnobStorage::ConsumesRelease()`, `HoldClaimed()` and
  `IndicatorColor()`.
- `ButtonBank` suppresses a tap whose release a used hold claimed and
  resolves its consume handshake through `ConsumesRelease()`. Hold gestures
  claim at the release rather than at fire time.
- `ControlLoop` paints navigation indicators per button through
  `IndicatorColor()`. It previously painted the page tint on button 0
  regardless of which button the pager used. A custom `KnobStorage` must
  implement `PageButton()` or override `IndicatorColor()` to get one.
- `ParamLock` banks its record gesture at the trigger's press edge, which
  fixes a `GoToPage` during a hold retargeting an in-flight recording. It
  holds the pager view latch (`RetainPage`, `ReleasePage`) and consumes a
  release only when its trigger carries a release-driven binding.
- Fixed: a pager button pressed under or across the Settings gate no longer
  advances the page on release.
- `ConsumeButton()` claims this frame's releases and the release of any
  bound button held when it is called. A claim made with no bound button
  held is discarded at the end of the frame.
- Added `docs/pages-and-layers.md` and `tests/host/pager_nav_tests.cpp`.

## Pot conditioning (#32)

- Added a 20 Hz one-pole filter on the V2 pots, applied at the 1 kHz
  `ProcessAllControls()` cadence and primed on the first ADC scan.
- Added a motion gate to pot catch, in a timed
  `UpdateCatch(state, phys, t_ms)` overload. A caught pot holds its stored
  value exactly while asleep, wakes on more than 0.4% of travel, tracks the
  knob 1:1 while awake, and sleeps after 400 ms within a 0.15% band.
  `Pager` and `Settings` call the timed overload. The untimed overload is
  unchanged.
- Added `tests/pot_gate_test.cpp`.

## ClockFollower (#31)

- `OnPulse()` writes to an 8-deep single-producer ring instead of a
  one-slot mailbox. Pulses are no longer dropped when a poll runs later
  than the pulse period. Fixes loss of lock on fast external clocks.
- Phase error is measured at the pulse timestamp through
  `MusicalClock::LastTickUs()`. `Update()` and `Tick()` may be called in
  either order.
- The PI target scales with the number of pulses drained per update.
- Added `tests/clock_follower_test.cpp`.

## Batched CV output (#28)

- Added `CvJack::StageVolts()` and `AlchemyLabV2::FlushCvOutputs()`.
  `StageVolts()` writes the shared MCP4728 shadow and sets a dirty flag.
  `FlushCvOutputs()` latches every channel with one WriteAll and one LDAC
  pulse, about 430 us, and returns immediately when nothing is staged.
- On J7 to J10 `StageVolts()` is identical to `SetVolts()`.
- `SetVolts()` is unchanged.
- Thanks to Nicholas Marrone.

## Builders reject braced-list temporaries (#33)

- `Selector`, `Labels`, `Colors`, `Buttons` and `Jacks` reject a braced
  list at compile time. The list was a temporary and the retained pointer
  dangled. Pass a named array.
- `VirtualButton::Colors(ptr, n)` no longer defaults `n`. `.Colors(kArray)`
  resolved to this overload without a count, skipping the zone-count check.

## Also

- Added `BrightnessHandle::At()` and `Settings::RelocateSlot()`, which move
  a settings slot to another page and pot.
- No descriptor or wire changes. `dv` stays 1.

# Alchemy SDK v0.10.0 — 2026-08-19

Modules can now ship their own documentation, and the descriptor build
stopped failing quietly.

## Interactive manual support (#21)

- Firmware can carry its own manual. `.Help()` attaches prose to knobs,
  buttons, settings, pages, and jacks; `.SeeAlso()` makes typed
  cross-references that resolve and validate at build time.
- `alchemy::Jack` (`surface/jack.h`): pure descriptor metadata for a panel
  jack: id, name, silk label, signal class, normalling. No runtime behavior,
  no state, no schema-hash impact.
- `alchemy::Manual` (`surface/manual.h`): module-level tagline, preamble, and
  long-form sections, plus `stock_help` for the features the SDK owns
  (presets, parameter locks, storage, brightness).
- `VirtualButton::GestureHelp(key, md)` keys help to the gesture string passed
  to `.Action()`; a key matching no declared gesture fails the build.
- Settings slots gain identity: `.Ident()`, `.Name()`, `.Help()`, labeled
  `Selector`, and per-page help.
- Descriptor rendering no longer depends on the order of calls in `main()`.
  Factory defaults are captured before the boot preset loads; the descriptor
  renders once everything is declared.
- A failed descriptor build reports its reason over the wire instead of
  returning zero, so a bad cross-reference surfaces in the host rather than as
  a module that silently claims to have no descriptor (protocol §5.2).
- Additive on the wire; `dv` stays 1. Prose never reaches any schema hash, so
  editing help text cannot invalidate a saved preset.

## Calibrated VDDA for CV input volts (#22)

- CV input voltage conversion uses the calibrated VDDA rather than the nominal
  value.

## ClipIndicator: SDK clipping light (#19)

- `alchemy::ClipIndicator` (`anims/clip_indicator.h`) — audio-fed clip light
  with a 98 % full-scale threshold, leaky-bucket transient suppression (a
  one-sample graze no longer flashes the panel), and a 50 ms minimum hold that
  retriggers while clipping persists. Feed `Process()` from the audio callback
  (ISR-safe single-word handoff); everything else is wired by `loop.Use(clip)`
  — detection ticks at the poll cadence, pips draw above the perf render.
- Declares like a VirtualKnob, with good defaults (every pot pip, red, 50 ms):

  ```cpp
  static ClipIndicator clip = ClipIndicator()
      .Hold(120.0f)
      .Pots(kPotTopLeft, kPotTopRight)
      .Buttons(kButtonB3);
  ```

  Also `Threshold`, `Suppression(samples, window_ms)`, `Accent`, `PipHour`,
  `Color`, `AllPots`, and `SetEnabled()` for a settings toggle. Bespoke loops
  call `Tick(dt_ms)` / `Draw(panel)` directly; `Draw(panel, color)` serves
  palette-driven modules.
- The sample scan runs as integer bit-pattern compares (abs = clear the sign
  bit; IEEE ordering is monotonic), so the audio-side cost is ~7 integer
  instructions per checked sample with no FPU flag transfers — and it probes
  at a stride derived from the suppression config (`min_clipped_samples / 2`,
  capped at 16; 4 for the default 8), with each hit counting as one stride of
  samples. The derivation keeps the suppression contract intact at any
  setting — a lone sub-stride burst can never reach the trigger — while the
  scan gets proportionally cheaper as suppression relaxes, and exact when it
  drops below 4. Non-finite samples (NaN/Inf) count as clipped — a broken
  signal lights the alarm.

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
