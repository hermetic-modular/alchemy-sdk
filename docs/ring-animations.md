# Ring animations

A ring rendering is a stack:

```
Ring = Base (exactly 1) + Overlays (0..N: Pip | Field) + catch pip
```

driven by plain floats — knob values, DSP telemetry, time phases,
constants. There are two overlay primitives. Everything that looks like a
different animation (cursor, comet, ping, playhead, notch, landmark,
shimmer, stutter, afterglow) is a parameterization of one of them, never a
new type.

Two ways to author a ring:

- **Declarative** — populate a `ParamSlot` (usually via `VirtualKnob`
  ring styles or the `LedRender` binder) and let `PerfRenderer` draw it.
  Right for rings that show only the knob value.
- **Compositional** — take the ring over (`ArcStyle::None`, or a
  `Custom` / `Overdraw` knob callback) and compose a `RingFrame`.
  The moment a ring needs a second signal, use this path.

Both run on the same rendering core.

## Rules

1. **The value never lies.** The Base is the only layer that encodes the
   control's value. Overlays modulate brightness and add elements; they
   never move or hide the value indication.
2. **Animate from truth.** Drive overlays from DSP state wherever
   possible — publish block-rate `volatile float`s from the audio
   callback and read them in the ring callback (see
   `examples/ring_demo`). A ring that moves when the audio moves is both
   clearer and better looking than wall-clock decoration. `t_ms` is for
   things that genuinely are clocks.
3. **Stateless by default.** Bases, Pips, and Fields are pure functions
   of `(values, t_ms)` — deterministic and frame-rate independent.
   Stateful animators (`Sparkle`, `BeatPip`) keep caller-owned state.

## RingFrame

```cpp
RingFrame f;
f.Begin(geo);                                  // own the ring (cleared)
f.Base(fill, value);                           // stamps the region map
f.Field(Region::Active, FieldShimmer(), wet_env, t_ms);
f.Pip(Region::Full, playhead, lag01);
f.Emit(panel, pot, pot_state);                 // quantize, catch pip last
```

- Accumulation is float RGB; one quantize + `ScaleGlobal` at `Emit`.
- Rendering is call order. Base first; the catch pip is painted by
  `Emit` and is always on top.
- `Begin()` owns the ring — `Emit` writes every LED. `BeginOverlay()`
  layers over an existing render — `Emit` writes only touched LEDs.
- Composition (`Add` / `Carve` pips, Fields) works on this frame's
  contents, not the panel — a frame cannot read pixels back. Draw the
  layer you want modulated into the same frame; in an overlay frame,
  `Carve` / `Field` on untouched LEDs is a no-op.
- The panel-direct `DrawFill` / `DrawPip` / `DrawSelector` functions are
  thin wrappers over this class.

## Regions

Stamped by the Base, consumed by overlays. Overlay positions are
region-relative 0..1 (0 = the region's CCW end) — never LED indices, so
compositions are portable across ring geometries.

| Region | Meaning |
|---|---|
| `Full` | the whole arc (always available) |
| `Active` | the lit value region — fill, both fan arms, or the selected zone |
| `Passive` / `PastTip` | from the fill tip to the CW end (Edge fills) |
| `ArmCw` / `ArmCcw` | the lit arm of a Center fan |
| `BottomPip` | the off-arc bottom LED (Pip only) |
| `Span` | the sub-span set by `SetSpan(a01, b01)` |

A region the current base cannot express resolves to empty; overlays
targeting it are no-ops.

## Bases (choose exactly one)

A contiguous lit arc (`Fill`), discrete dots (`Selector`), or a
color-morphing fill (`Gradient`).

### `Fill` (`FillDesc`)

`Edge` grows from one end of the ring — the standard "how much" bar.
`Center` fans out from a pivot in two arm colors — "which way and how
far".

| Param | Type / default | Meaning |
|---|---|---|
| `value` (call arg) | float 0..1 | the control value; both modes |
| `mode` | `Edge` | `Edge` or `Center` |
| `direction` | `Cw` | Edge only: which stop the fill grows from |
| `pivot01` | `0.5` | Center only: fan origin in value space; each arm normalized to its own side |
| `color` | white | fill / CW arm |
| `neg_color` | black = same as `color` | CCW arm (Center) |
| `passive_color` | black | unlit region (Replace compose) |
| `center_color` | white | pivot LED (Center) |
| `anim`, `anim_depth` | `None`, `0.4` | declarative `Pulse` / `Ripple` |
| `compose` | `Replace` | `Overlay` writes active LEDs only |

### `Selector` (`SelectorDesc`)

N discrete zones, one lit bright. Use `Base(desc, value)` to derive the
zone from a 0..1 value, or `BaseZone(desc, zone)` when the caller runs
its own zone map (hysteresis, custom boundaries).

| Param | Type / default | Meaning |
|---|---|---|
| `num_zones` | 2 | zone count |
| `zone_geo` | `Distributed` | `Distributed` / `Point` / `Region` placement |
| `active_color` | white | selected zone |
| `inactive_color`, `inactive_dim` | black, 0.10 | other available zones |
| `avail_mask` | all | unavailable zones render black |

### `Gradient` / `GradientFill` (`GradientDesc`, declarative path)

A filled arc whose color morphs across snap points as the value moves;
`GradientFill` tints a plain fill from a sibling pot's value. Configured
on `ParamSlot.gradient`; rendered by `PerfRenderer`.

## Overlays

### `Pip` (`PipDesc`) — a positioned element

One small point of light (or darkness) on top of the base. A moving
position makes a cursor, comet, ping, or playhead; a constant position
makes a landmark tick; `Carve` makes a notch; a long one-sided trail with
a dim core makes an afterglow.

```cpp
f.Pip(Region::Active, desc, pos01 /*, gain = 1, t_ms = 0 */);
```

| Param | Type / default | Meaning |
|---|---|---|
| `pos01` (call arg) | float | position in region space; constant = landmark, signal = motion |
| `gain` (call arg) | float 0..1 | brightness (Carve: cut depth) |
| `compose` | `Replace` | `Replace` owns its LEDs · `Add` rides on top (saturating) · `Carve` dims what is drawn · Add/Carve compose against this frame only |
| `motion` | `Direct` | `Direct` clamps · `Wrap` laps (cursor) · `Bounce` ping-pongs (comet) |
| `color` | white | element color |
| `width` | 1 | LEDs; >1 disables `smooth` |
| `smooth` | false | sub-LED two-tap interpolation — continuous glide |
| `tail_intensity` | 0 | one-step comet tails (sugar for a 1-step trail) |
| `trail_steps` | 0 | trail length in LEDs |
| `trail_falloff` | 0.5 | per-LED trail decay |
| `trail_side` | `Both` | `Both` / `Ccw` / `Cw` |
| `blink_hz` | 0 | 0 = solid |
| `background` | black = none | dim region fill under the pip |

`Carve` composes against the current frame contents, so carve into a base
drawn in the same frame.

### `Field` (`FieldDesc`) — a region-wide modulation

Not an element — a texture in the brightness of an already-lit region.
The LEDs keep their color; their level breathes, ripples, twinkles,
stutters, or bands.

```cpp
f.Field(Region::Active, desc, amount /*, t_ms = 0, phase01 = -1 */);
```

| Param | Type / default | Meaning |
|---|---|---|
| `amount` (call arg) | float 0..1 | modulation depth; drive with an envelope for "alive only when audible" |
| `phase01` (call arg) | −1 | explicit phase for signal-driven motion; negative = derive from `t_ms` |
| `pattern` | `Uniform` | `Uniform` (same everywhere) · `Noise` (hash per unit) · `Wave` (drifting wave) |
| `grain` | `Whole` | `Whole` · `PerLed` · `Chunk` — spatial coherence |
| `step` | `Smooth` | `Smooth` · `Held` (sample-and-hold jumps) |
| `rate_ms` | 1000 | period (Smooth) or hold bucket (Held) |
| `chunk_leds` | 2 | band width (Chunk) |
| `spatial_leds` | 4 | Wave wavelength; sign sets drift direction |
| `invert` | false | lift from a floor instead of dipping |

Presets (parameter combinations, not types): `FieldPulse()` ·
`FieldRipple()` · `FieldShimmer()` · `FieldStutter()` ·
`FieldStaircase()`. The declarative `FillAnim::Pulse` / `Ripple` map onto
the same math.

## Signals

| Helper | Purpose |
|---|---|
| `Phase01(t_ms, period_ms)` | sawtooth clock phase |
| `Tri01(phase)` | triangle fold of a phase |
| `Hash01(unit, bucket)` | deterministic per-unit texture value |
| `NormTapered(x, floor, ceiling, taper)` | display mapping for wide-range signals (below floor → negative = hide; above ceiling → pegged) |
| `LedPanel::Mix(a, b, t)` | signal-driven color |

## Stateful animators

`Sparkle` (spawn/decay scatter across the ring) and `BeatPip`
(tempo-synced bottom pip) keep caller-owned state and compose alongside
the stack. See `anims/sparkle.h` and `anims/beat_pip.h`.

## ParamSlot migration (pre-composition firmwares)

`ParamSlot` embeds the primitive descriptors instead of flattened copies
of their fields:

| Old field | New location |
|---|---|
| `arc_color` | `fill.color` / `selector.active_color` |
| `arc_alt_color` | `fill.passive_color` (Level) / `fill.neg_color` (Bipolar) / `selector.inactive_color` (Selector) |
| `arc_center_color` | `fill.center_color` |
| `arc_anim`, `arc_anim_depth` | `fill.anim`, `fill.anim_depth` |
| `arc_zone_geo`, `arc_num_zones`, `arc_inactive_dim`, `arc_avail_mask` | `selector.*` |
| `arc_snaps`, `arc_num_snaps`, `arc_color_src_pot` | `gradient.*` |
| `pip_style`, `pip_color`, `snap_lo`, `snap_hi`, `snap_hi_color` | `pip.*` |

`FillDesc.pivot01` (renamed from `pivot`) changed from a step index to
the 0..1 value space; the Bipolar arc's value is now the control's 0..1
norm (the ±1 conversion lives only in the legacy `DrawFill` entry
point). Code using the `VirtualKnob` ring styles or the binder sugar
setters compiles unchanged, and `ParamSlot`'s embedded descriptors
default to the old flattened-field values (mid-gray arc, 8 zones,
inactive zones off), so declarative rings render as before.

Intentional rendering changes vs. the pre-composition renderer:

- An Edge fill's tip LED at an exact step boundary now shows
  `passive_color` instead of black.
- `ArcStyle::GradientFill` now honors `fill.anim` and
  `fill.passive_color`.
- Bipolar arms normalize per-side, so both ends are reached at the pot
  extremes for any pivot (identical output at the default midpoint
  pivot).
- A declarative Selector with `num_zones == 0` blanks the ring instead
  of leaving stale pixels.
