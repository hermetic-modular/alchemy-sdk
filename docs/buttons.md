# Buttons: VirtualButton + ButtonBank

A button that cycles a filter mode touches five concerns: edge polling,
persistence, HostLink description, LED feedback, and the DSP-side
effect.  Hand-rolled, those are five separate pieces of code that must
agree on one field id and one byte layout, and nothing checks that they
do.  `VirtualButton` + `ButtonBank` collapse them into one declaration:

```cpp
static const char* kModes[3] = {"LP", "BP", "HP"};

static VirtualButton flt = VirtualButton(kButtonB3, "Filter")
    .Ident("flt.mode")         // stable host id (default "b<hw>")
    .Selector(kModes)          // 3 zones + labels from one array
    .Colors(kModeColors)       // per-zone LED feedback
    .Bind(SetFilterMode);      // fires on gesture and preset load

static Page       page_a = Page(0).Knobs(k1, k2).Buttons(flt);
static ButtonBank buttons;

int main()
{
    ...
    presets.Manage(buttons);   // one byte per stateful button
    loop.Use(pager).Use(page_a).Use(buttons).Use(host);
    ...
}
```

Everything else is derived: with no declared gesture the button
tap-cycles (two zones read as a toggle), the browser shows an editable
"Filter" enum inside page Alpha labeled "Cycle LP / BP / HP", presets
carry the zone, and `Bind` keeps `SetFilterMode` current on gestures
and preset loads alike.  `flt.Zone()` is the pull-style read for code
that prefers polling in `OnFrame`.

## The model

`VirtualButton` is the twin of `VirtualKnob`, and `ButtonBank` is its
`Pager`: the surface that owns the state.  The declared object is pure
description; the bank persists, dispatches, renders, and describes.

One deliberate asymmetry: knob state is keyed by *(page, pot)* because
a physical pot has a position that must be reconciled per page — that
is what pot-catch exists for.  A button has no position, so state is
keyed by the **VirtualButton object**.  Pages are pure dispatch scope:

- The same physical button does different things on different pages by
  declaring one VirtualButton per page (`page_a.Buttons(flt)`,
  `page_b.Buttons(wave)` — both constructed on `kButtonB3`, each with
  its own `.Ident()`).
- The same object on several pages is deliberately one shared control
  (one preset byte, rendered on each page).
- Moving a button between pages at runtime redirects dispatch and
  never touches its state.

`ButtonBank::Global()` registers a button active on every page.

## State, gestures, effects

| Declaration | Meaning |
|---|---|
| `VirtualButton(hw, name)` | physical button (e.g. `kButtonB1`; a bare literal `0` won't compile — use the constant) |
| `.Ident("field.id")` | stable host id; defaults to `"b<hw>"` — required when two buttons share a hw index |
| `.Selector(kLabels)` | N-zone state; zones + labels from one array |
| `.Selector(n)` / `.Toggle()` | unlabeled N zones / 2 zones |
| `.Default(z)` | factory zone (checked against N) |
| *(no Selector)* | momentary: zero preset bytes |
| `.Tap(Action::Cycle, "…")` | explicit tap; label optional (derived) |
| `.TapSet(z)` / `.HoldSet(ms, z)` | jump to a zone |
| `.Hold(ms, Action::…)` | hold mutation; suppresses the trailing tap |
| `.Tap(fn)` / `.Hold(ms, fn)` | momentary callbacks |
| `.Bind(&var)` / `.Bind(Setter)` | write zone into a variable / setter |
| `.OnChange(fn, ctx)` | general notification, after Bind targets |
| `.Anchor("field.id")` | attach to that field in the browser: renders with it wherever it appears, falls back to the page's button group where it doesn't; the id must name an emitted field or the descriptor build fails |
| `VirtualButton(ident, name)` | host-only state: persisted + editable, no gesture |

Gesture callbacks and mutations run in the control-loop poll (the
`OnPoll` context).  Preset loads apply data in `Deserialize` but defer
the `Bind`/`OnChange` notifications to the next frame, coalescing
HostLink live pushes; notifications fire only when the zone actually
changed.

## The contract

- **Edges are never stolen.**  The bank reads only `IButton::Pressed()`
  and derives its own edges.  App code reading `RisingEdge()` directly
  (the `kick` example) keeps every edge — and sample-accurate triggers
  like kick's audio-callback drum trigger should *stay* on direct
  reads; bank callbacks run at the 1 ms poll.
- **Gestures resolve against the press-time page.**  A page change mid
  hold cannot retarget the gesture.
- **Pager shadowing is deliberate.**  A gesture fired on the pager's
  button consumes that release (`KnobStorage::ConsumeButton`), so a
  button declared on B1 shadows page-advance on pages that declare it —
  and only there.
- **Don't stack holds on the ParamLock button.**  Lock recording is
  hold+knob; the two hold gestures cannot be arbitrated.
- **Settings gates gestures, not data.**  While Settings is active
  presses are inert (an in-flight press is discarded), but preset/host
  pushes still reach `Bind` targets.  One edge: the B2+B3 enter chord
  only claims its buttons once it *completes* (after `hold_ms`), so a
  tap declared on B2 or B3 fires if the chord is released early — a
  press that spans the activation is discarded by the gate, but an
  abandoned chord taps.  Harmless for cyclic state (one extra press
  undoes it); put tap-critical actions on buttons outside the chord.
- **LED precedence.**  The bank paints after the pager's page tint, so
  a declared mode color wins on a shared button.

## Freeze and validity

The roster — which buttons persist, in what byte order — freezes at the
first `Presets`/descriptor walk, the same moment the rest of the blob
layout freezes.  Declare every stateful button (via a page or
`Global()`) before `presets.BootLoad()`; page *membership* stays freely
mutable afterwards.

Declaration errors latch `Ok() == false` and fail the descriptor build
("no descriptor" beats a wrong one): label/color counts that disagree
with the zone count, `Default()` out of range, duplicate idents, fewer
than two zones, or a stateful button first seen after freeze.  An
`Anchor()` that names no emitted field (or the button's own id) fails
the same way — checked at descriptor `Finish()`, since the target may
live in a component managed later.  `DescriptorBuilder::LastError()`
carries the reason for any of these.

The bank's `SchemaHash()` folds every cell's ident and zone count, so
reshaping the roster (or renaming an ident) invalidates saved preset
slots instead of misreading them.  Adding a bank to existing firmware
is a `Manage()` addition and invalidates old slots like any other.

## HostLink

The bank emits one `kind: "buttons"` component (protocol §5.5).
Stateful buttons are ordinary 1-byte enum fields, so **any** host —
including one that predates the kind — renders them as an editable
group and can change them today; button-aware hosts use the `page`,
`anchor`, and `btn` keys to draw them inside the page cards.  Momentary
buttons emit into the component's `modal` array.

Modules using the bank simply never call `Host::Buttons()`, so the
legacy root `buttons` array (§5.3) is absent — no duplicate rendering.
The legacy array remains supported for metadata-only firmware.

## Loop-less use

Without a `ControlLoop`, wire explicitly and drive the three phases
yourself: `bank.Attach(btns, n).Pages(pg)`, then per millisecond
`bank.PollButtons(t_ms, gated, active_page)`, per frame
`bank.Update(t_ms)` and `bank.Render(panel, active_page, t_ms)`.

## Deferred by design

- **Double-tap** — recognizing it forces latency onto every plain tap
  on that button; it will be added when a real module wants it.
- **External field binding** (two buttons mutating one shared field
  with different actions) — the cell layer is already shaped for it;
  today, declare the second button momentary and call
  `bank.SetZone(first, z)` from its callback.
