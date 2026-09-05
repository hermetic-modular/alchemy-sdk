# Alchemy SDK — unreleased

Shift layers are pages: page navigation became declarative button
bindings on `Pager`, so a hold-layer gets pot-catch, presets, locks, CV,
rings, and the descriptor for free (#16).

## Pager navigation bindings

- `Pager(num_pages, num_pots)` + fluent `.Cycle(btn, pages...)`,
  `.Shift(btn, page)`, `.Latch(btn, a, b)`, `.From(pages...)`.  The
  classic `Pager(b1, n, m)` constructor is unchanged and equivalent to
  `.Cycle(b1)` over every page; the serialized image and `'PAG0'` schema
  are untouched.
- Shift engages on the press edge and re-arms catch both ways; catch
  only ever runs on the active page, so a hold cannot smear the pages
  underneath — no app-side snapshot/restore.
- The clean-release contract: a hold that edited nothing leaves the
  release for taps; a used hold claims it.  `Pager::HoldUsed()` is the
  pull-style read.
- Latch pairs are sub-pages that occupy one cycle slot and remember
  their member (the issue-#16 VAR/RND UI).
- Arbitration built in: gate (Settings) poisoning incl. presses spanning
  the gate, chord-reach abort on nav-button co-press, one layer at a
  time, `GoToPage` cancels an active layer.
- `KnobStorage` gains three defaulted queries — `ConsumesRelease()`
  (the general form of the `PageButton()` identity check),
  `HoldClaimed()`, and `IndicatorColor()`.
- `ButtonBank` suppresses a tap whose release a used hold claimed, and
  resolves its consume handshake through `ConsumesRelease()` — so latch
  buttons shadow correctly too.  Hold gestures now make their consume
  claim at the release (same-frame contract) rather than at fire time.
- `ControlLoop` paints navigation indicators per button via
  `IndicatorColor()`.  (It previously painted the page tint on button 0
  regardless of which button the pager was constructed on.  A custom
  `KnobStorage` that wants an indicator must implement `PageButton()`
  or override `IndicatorColor()`.)
- `ParamLock` banks its record gesture at the trigger's press edge —
  fixes a latent bug where a mid-hold `GoToPage` silently retargeted an
  in-flight recording — holds the pager's view latch
  (`RetainPage`/`ReleasePage`) so a recording started on a layer keeps
  its surface, and consumes only when its trigger actually carries a
  release-driven binding.
- Behavior fix: a pager-button press alive under (or spanning) the
  Settings gate is poisoned until released, and an edge already latched
  when the gate rises is dropped; previously the release after the gate
  lifted still advanced the page.  Layers abort the instant the gate
  rises, so Settings opens over the base surface.  The consume claim is
  now scoped: it covers this frame's releases and the release of any
  bound button held when the claim is made (the mid-hold chord idiom),
  and otherwise evaporates instead of latching onto a later, unrelated
  release.
- New: `docs/pages-and-layers.md` and a host test battery
  (`tests/host/pager_nav_tests.cpp`).

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
