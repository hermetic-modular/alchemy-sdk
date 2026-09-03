# Pages and layers: Pager navigation bindings

A shift layer touches everything a page touches: pot values and their
catch state, presets, param-locks, CV routing, ring rendering, and the
HostLink descriptor.  Hand-rolled as a parallel surface, each of those
integrations must be rebuilt and kept honest by hand — snapshotting the
pager's storage around every hold, compensating the DSP reads for catch
smear, re-implementing `LockSource` to bank locks per layer, writing a
`Serializable` and a `Describe` for the layer's values.

The SDK's answer is to not have layers as a second concept.  **A layer
is a page selected by holding a button.**  `Pager` owns the page pool it
always owned; what's new is that page *navigation* is declared, per
button:

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

`presets.Manage(pager)` persists the layer's values with everything
else; `ParamLock` banks per page, so the layer records automation; a
`CvMatrix` routes to `(page, pot)` cells, so the layer takes CV; the
renderer draws the layer's declared rings while it's held; the web
editor shows it as a page tab.  None of that is layer-specific code.

The classic constructor `Pager(b1, n_pages, n_pots)` is unchanged and
means exactly `Pager(n_pages, n_pots).Cycle(b1)` — an empty cycle list
is "every page, in order".

## The three bindings

| Declaration | Gesture | Meaning |
|---|---|---|
| `.Cycle(btn, pages...)` | clean release | advance to the next entry (all pages when the list is empty) |
| `.Shift(btn, page)` | hold | that page while held; return on release |
| `.Latch(btn, a, b)` | clean release | toggle the sub-page pair while one member is showing |
| `.From(pages...)` | — | scope the binding declared just before it (Shift/Latch) |

One binding per button, at most four bindings, at most one Shift layer
active at a time.  Out-of-range pages reject the declaration silently,
matching `GoToPage`'s out-of-range rule.

**Shift engages on the press edge** — no hold threshold.  The moment the
button goes down, the layer's page re-arms pot-catch (`LockPage`), so
every pot detaches and must cross its stored value to take hold; early
movement can never drag a base parameter.  The release returns to the
page the press left and re-arms catch again — the pot is wherever the
layer edit parked it, and the base value waits to be caught.  Because
catch only ever runs on the active page, the pages underneath a hold are
untouched *by construction*: there is nothing to snapshot and nothing to
restore.

**Latch pairs are sub-pages** (the [issue #16](https://github.com/hermetic-modular/alchemy-sdk/issues/16)
UI).  `Latch(b3, kVar, kRnd)` toggles between the two while either is
showing and is inert elsewhere — the button stays free for `ButtonBank`
work on other pages.  A pair occupies **one cycle slot**: with
`Cycle(b1, kVar, kReverb)`, standing on `kRnd` still advances to
`kReverb`, and cycling back lands on whichever member the pair last
showed.  A page may belong to at most one pair.

## The release contract

**Clean releases compose with taps.**  A Shift hold that edited nothing
(no pot caught, no caught pot moved past the catch tolerance) does not
claim its release.  A hold that did edit claims it: `ButtonBank` asks
the storage (`KnobStorage::HoldClaimed`) before firing a tap, so

```cpp
static VirtualButton mute_btn = VirtualButton(kButtonB2, "Mute")
    .Ident("out.mute").Toggle().Bind(SetMute);
```

on the same button as `.Shift(...)` gives the kick idiom — tap mutes,
hold-and-turn edits the layer — with no app-side arbitration.  App code
outside the bank reads `pager.HoldUsed(btn)` instead; it stays valid
across the release frame.  The release frame itself never absorbs pot
motion (releases process before that frame's catch), so a
final-millisecond nudge cannot both edit the layer and pass for a clean
tap.

**Consume is scoped and generalized.**  A surface firing its own
gesture on a button that carries a release-driven binding (Cycle *or*
Latch — test with `KnobStorage::ConsumesRelease`) calls
`ConsumeButton()`, and that release navigates nothing.  The claim covers
this frame's releases and the release of any bound button held when the
claim is made, so a chord handler may claim the moment the chord forms.
A claim with nothing held evaporates at the end of the frame instead of
latching onto a later, unrelated release.  Consumers still run before
the pager in the canonical order (`locks.Update` → `pager.Update`).

## Arbitration

These rules exist because every hand-rolled layer implementation ended
up needing them:

- **Gate (Settings).**  While gated, bindings sync but fire nothing.  A
  press alive under — or spanning — the gate is poisoned until its
  release, and the gate rising mid-hold aborts an active layer, so the
  Settings chord always lands on a clean base surface.
- **Chord reach.**  Two navigation-bound buttons down at once reads as
  reaching for a chord: active layers abort and both presses poison for
  their whole holds.  (A layer on a Settings-chord button whose partner
  is *not* navigation-bound is briefly active during the reach; the
  gate abort cleans it up when Settings opens.)
- **View latch.**  `ParamLock` holds `Pager::RetainPage()` around its
  trigger hold: a layer showing when the hold began defers its exit —
  rings, catch, and the recording's bank stay in agreement — until the
  trigger releases.  Layers engaged *after* the hold began aren't
  covered.
- **`GoToPage` outranks everything**: an explicit jump cancels an active
  layer with no return leg and adopts the target into its latch pair's
  memory if it has one.

## Locks bank at the press edge

`ParamLock`'s record gesture latches its slot bank when the trigger goes
down — you lock what you were looking at.  A layer released mid-hold, or
a host-driven `GoToPage`, cannot retarget an in-flight recording.  (This
also fixes a pre-binding latent bug: the bank used to resolve live, so a
`GoToPage` during a hold silently switched the recording's target.)

## LEDs

`ControlLoop` queries `KnobStorage::IndicatorColor(btn)` once per button
per frame: the cycle button wears the active page's color (as before), a
Shift button wears its layer's color while engaged, a Latch button wears
the showing member's color while on its pair.  `ButtonBank` zone colors
still paint afterwards and deliberately win.

## What stays app-side

- **Zone-commit debouncing** for selector knobs that trigger expensive
  DSP swaps (hysteresis + dwell) — a DSP-commit policy, not a surface
  concern.
- **Custom ring art** for layer faces — declare per-knob
  `.Ring(Custom(...))` / `.Overdraw(...)` as on any page.
- **Trigger suppression** while a layer is held (e.g. a trigger pad
  sharing the panel) — read `pager.ActivePage()` or `HoldUsed()`.

## Deferred by design

- **Tap-to-latch / hold-to-momentary hybrids** on one Shift button —
  recognizing the split forces latency onto every plain interaction;
  the binding model leaves room when a real module wants it.
- **Cross-products** (a shift of a shift) — nothing motivates it.
- **Multiple `KnobStorage`s per loop** — `(page, pot)` is the keying
  invariant across locks, CV, rendering, presets, and the descriptor;
  a second storage forks it everywhere.  Layers-as-pages is the answer.
- **Descriptor metadata for navigation gestures** ("hold B2 for Shift"
  in the editor and the interactive manual) — additive, follows the
  `gestures` precedent from #17.
