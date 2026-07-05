# Alchemy Lab examples

| Folder | What's inside | What it demonstrates |
|---|---|---|
| [`stereo_eq/`](stereo_eq/) | A dual mono three-band EQ built on the full opt-in surface stack. | The framework's full feature set — pagination, presets, param-lock, CV routing, and declarative arc styling — composed into one audible module. |
| [`kick/`](kick/) | A trigger-driven kick drum: pitch-swept sine body, switchable transient layer, ParamLock-recorded automation. | A synthesis (instrument) module rather than a processor — how ParamLock and LedRender compose with a hand-rolled DSP core and a live signal-driven meter overdraw. |
| [`clock_sync/`](clock_sync/) | External 24 PPQN clock follower amplitude-modulating an audio passthrough. | Minimal `MusicalClock` + `ClockFollower` wiring for phase-aware tempo sync. |
| [`ring_demo/`](ring_demo/) | A stereo tremolo whose rings render the LFO itself: a comet pip on the rate ring, a carved notch + shimmer on the depth ring. | Ring composition — `RingFrame`, `Pip`, `Field`, and the publish-DSP-state pattern for signal-driven animation. |
| [`v2_cal_test/`](v2_cal_test/) *(V2 only)* | Scope-driven calibration acceptance test: B1 steps all jacks −5..+5 V, B2 toggles calibration on/off, the rings show the target voltage. | The calibrated CV-out API end-to-end, plus the LED panel as a bench instrument. |
| [`v2_cv_demo/`](v2_cv_demo/) *(V2 only)* | All six jacks auto-cycling ±4 V with cal status on the Seed LED. | The minimal calibrated CV-out wiring — `RouteCvOut` + `SetCvOutVolts` in ~60 lines. |

---

## `stereo_eq/` — dual mono EQ on the full surface stack

[`stereo_eq.cpp`](stereo_eq/stereo_eq.cpp) links `alchemy::surface` and
opts into the whole stack: Pager + ParamLock + Settings + CvRouter +
LedRender + Presets + PresetGestureUi — every feature is a constructor
call.  Three EQ bands per channel with a musically-spaced frequency
sweep and ±24 dB of cut/boost.  The biquad math lives in its own
translation unit ([`stereo_eq_dsp.cpp`](stereo_eq/stereo_eq_dsp.cpp))
and the LED palettes in
[`stereo_eq_palette.h`](stereo_eq/stereo_eq_palette.h) so
`stereo_eq.cpp` stays a pure SDK example.

### Dual mono across two pages

Both performance pages expose a fully isolated mono EQ per channel:

- **Page 0** processes the left channel only — cool palette (cyan / teal /
  deep blue).
- **Page 1** processes the right channel only — warm palette (orange / red /
  magenta).

Each page has six pots with the same layout (`level` left column, `freq`
right column, `hi / mid / lo` top to bottom).  CV routing is declared once
with the same quarter-turn-CCW mapping.  The two channels share no
parameters — they are two complete EQs that happen to live in one module.

---

## `kick/` — a synthesis module with ParamLock + LedRender

A single-page percussion instrument: pitch-swept sine body, hard-clip
saturation, and a switchable one-shot transient layer (none / click / tick /
knock).  Contrasts the `stereo_eq` family — those are processors that
modify an input signal, whereas this one generates audio in response to a
trigger button, but more importantly, opts to NOT use many SDK features 
and implements non-standard (non-maximal) uses of the hardware.

| Pot | Function                              | Arc style |
|-----|---------------------------------------|-----------|
| P1  | Trigger pitch                         | Level (orange) + Pulse |
| P2  | Sweep speed                           | Level (magenta) + Ripple |
| P3  | Transient zone (none / click / tick / knock) | 4-zone Selector (Region) |
| P4  | Drive                                 | Bipolar (lime ↔ cyan) |
| P5  | Decay (snap-pip at unity 0.45–0.55)   | Level (violet) + ThresholdSnap pip |
| P6  | Output volume                         | Level (dim) + live envelope meter |

`B2` triggers the kick (rising edge).  `B1` is held while nudging a pot to
record a `ParamLock` loop on that pot — exactly the gesture the EQ example
uses on its dual-button layout, but with the trigger button broken out to
B2 so the kick can be fired with a single finger.

What the example demonstrates:

- **Synthesis vs. processing.**  A trigger flag is shared between the
  control loop and the audio callback; the callback drains it once per
  block and resets the envelope on rising edges for sample-accurate
  retrigger.
- **Declarative arc styling everywhere except the meter.**  `LedRender`'s
  fluent `Binder` API declares all six rings (Pulse, Ripple, Selector,
  Bipolar, ThresholdSnap pip, dim base) once at startup.  The control loop
  then renders `LedRender` first, overdraws a green / yellow / red live
  envelope meter on P6 with `DrawLevelArc`, and finally lets `ParamLock`
  paint its red-recording / green-active pips on top.
- **ParamLock without a Pager.**  `ParamLock<6>` in its unpaged form
  records six independent loops — one per pot — that are layered into the
  pot reads at a single composition site (`Knob()`).

Only `ParamLock` and `LedRender` are linked — `Pager`, `Settings`,
`Presets`, and `CvRouter` are not in the binary, because a single-page
percussion module doesn't ask for them.
