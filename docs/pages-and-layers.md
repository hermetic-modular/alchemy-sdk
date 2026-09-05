# Pages and layers: Pager navigation bindings

A shift layer touches everything a page touches: pot values and catch
state, presets, param-locks, CV routing, ring rendering, and the
HostLink descriptor. A layer implemented as a separate surface has to
reimplement each of those integrations.

In the SDK a layer is not a separate concept. **A layer is a page
selected by holding a button.** `Pager` owns the page pool, and page
*navigation* is declared per button:

```cpp
enum : uint8_t { kPageTone, kPageSpace, kPageShift, kNumAppPages };

static Pager pager = Pager(kNumAppPages, kNumPots)
    .Cycle(hw.buttons[kButtonB1], kPageTone, kPageSpace) // release advances
    .Shift(hw.buttons[kButtonB2], kPageShift);           // page while held
```

Knobs on the layer are ordinary `VirtualKnob`s on an ordinary `Page`:

```cpp
static VirtualKnob out_level = VirtualKnob(kPotTopLeft, "Out Level")
    .Linear(-40.f, 6.f).Unit("dB").Ident("out.level")
    .Ring(Level(kShiftViolet));

static Page shift_page = Page(kPageShift).Name("Shift").Knobs(out_level, ...);
```

- `presets.Manage(pager)` persists the layer's values with everything
  else.
- `ParamLock` banks per page, so the layer records automation.
- A `CvMatrix` routes to `(page, pot)` cells, so the layer takes CV.
- The renderer draws the layer's declared rings while it is held.
- The web editor shows it as a page tab.

None of that is layer-specific code.

`Pager(b1, n_pages, n_pots)` is equivalent to
`Pager(n_pages, n_pots).Cycle(b1)`. An empty cycle list means every page
in order.

## Bindings

| Declaration | Gesture | Meaning |
|---|---|---|
| `.Cycle(btn, pages...)` | clean release | advance to the next entry (all pages when the list is empty) |
| `.Shift(btn, page)` | hold | that page while held; return on release |
| `.Latch(btn, a, b)` | clean release | toggle the sub-page pair while one member is showing |
| `.From(pages...)` | n/a | scope the binding declared just before it (Shift/Latch) |

One binding per button, at most four bindings, at most one Shift layer
active at a time. Out-of-range pages reject the declaration silently,
matching `GoToPage`'s out-of-range rule.

**Shift engages on the press edge**, with no hold threshold. On press,
the layer's page re-arms pot-catch (`LockPage`): every pot is detached
until it crosses its stored value, so motion during the press cannot
move a base-page parameter. On release, the pager returns to the page
the press left and re-arms catch again, because the pot now sits where
the layer edit left it. Catch runs only on the active page, so pages
under a hold are never modified. No snapshot or restore is needed.

**Latch pairs are sub-pages** (the [issue #16](https://github.com/hermetic-modular/alchemy-sdk/issues/16)
UI). `Latch(b3, kVar, kRnd)` toggles between the two while either is
showing and does nothing elsewhere, so the button remains available to
`ButtonBank` on other pages. A pair occupies **one cycle slot**: with
`Cycle(b1, kVar, kReverb)`, the cycle advances from `kRnd` to
`kReverb`, and cycling back lands on whichever member the pair last
showed. A page may belong to at most one pair.

## The release contract

**A clean hold does not claim its release.** A Shift hold counts as
clean when no pot was caught and no caught pot moved past the catch
tolerance. A hold that did edit claims its release: `ButtonBank` asks
the storage (`KnobStorage::HoldClaimed`) before firing a tap, so

```cpp
static VirtualButton mute_btn = VirtualButton(kButtonB2, "Mute")
    .Ident("out.mute").Toggle().Bind(SetMute);
```

on the same button as `.Shift(...)` gives tap-to-mute plus
hold-and-turn-to-edit, as in the kick example, with no app-side
arbitration. App code outside the bank reads `pager.HoldUsed(btn)`
instead; it stays valid across the release frame. The release frame
itself never absorbs pot motion (releases process before that frame's
catch), so a final-millisecond nudge cannot both edit the layer and pass
for a clean tap.

**Consuming a release.** A surface firing its own gesture on a button
that carries a release-driven binding (Cycle *or* Latch; test with
`KnobStorage::ConsumesRelease`) calls `ConsumeButton()`, and that
release navigates nothing. The claim covers this frame's releases and
the release of any bound button held when the claim is made, so a chord
handler may claim the moment the chord forms. A claim made with nothing
held expires at the end of the frame, so it cannot attach to a later,
unrelated release. Consumers run before the pager: `locks.Update` then
`pager.Update`.

## Arbitration

- **Gate (Settings).** While gated, bindings sync but fire nothing. A
  press that starts under or spans the gate is poisoned (inert until its
  release). A gate rising mid-hold aborts an active layer, so Settings
  always opens on the base page.
- **Chord reach.** Two navigation-bound buttons down at once is treated
  as a chord attempt: active layers abort and both presses are poisoned
  for their whole holds. (A layer on a Settings-chord button whose
  partner is *not* navigation-bound stays active during the reach; the
  gate abort clears it when Settings opens.)
- **View latch.** `ParamLock` calls `Pager::RetainPage()` for the
  duration of its trigger hold. A layer active when the hold began stays
  active until the trigger releases, so rings, catch, and the
  recording's bank remain consistent. Layers engaged *after* the hold
  began are not retained.
- **`GoToPage` has priority.** An explicit jump cancels an active layer
  without returning to the previous page. If the target belongs to a
  latch pair, it becomes that pair's remembered member.

## Locks bank at the press edge

`ParamLock`'s record gesture latches its slot bank when the trigger goes
down: the recording targets the page visible at press time. A layer
released mid-hold, or a host-driven `GoToPage`, cannot retarget an
in-flight recording.

## LEDs

`ControlLoop` queries `KnobStorage::IndicatorColor(btn)` once per button
per frame: the cycle button shows the active page's color, a Shift
button shows its layer's color while engaged, and a Latch button shows
the visible member's color while on its pair. `ButtonBank` zone colors
paint afterwards and take precedence.

## What stays app-side

- **Zone-commit debouncing** for selector knobs that trigger expensive
  DSP swaps (hysteresis plus dwell). This is a DSP-commit policy, not a
  surface concern.
- **Custom ring art** for layer faces. Declare per-knob
  `.Ring(Custom(...))` / `.Overdraw(...)` as on any page.
- **Trigger suppression** while a layer is held (e.g. a trigger pad
  sharing the panel). Read `pager.ActivePage()` or `HoldUsed()`.

## Not implemented

- **Tap-to-latch / hold-to-momentary hybrids** on one Shift button.
  Detecting the split adds latency to every plain tap. The binding
  model can accommodate it later.
- **Nested shifts** (a shift of a shift). No use case.
- **Multiple `KnobStorage`s per loop.** `(page, pot)` is the keying
  invariant across locks, CV, rendering, presets, and the descriptor; a
  second storage would fork it everywhere.
- **Descriptor metadata for navigation gestures** ("hold B2 for Shift"
  in the editor and the interactive manual). Additive; follows the
  `gestures` precedent from [issue #17](https://github.com/hermetic-modular/alchemy-sdk/issues/17).
