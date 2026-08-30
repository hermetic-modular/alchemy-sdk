# Parameter locks: length, rate, behavior, and what they cost

A parameter lock records a knob gesture and loops it. Recording arms the
moment the knob moves (a 0.5% nudge), a firmer ~3% nudge on a knob that
is already looping clears it, and an optional **Clocked** mode restarts
every new loop on the musical grid. Capacity is a compile-time
declaration.

How long a gesture can be is your application's decision, declared once
at the ParamLock declaration:

```cpp
alchemy::ParamLock<6>                       locks(hw.buttons[0]);  // 16 s (default)
alchemy::ParamLock<6,  LockLength<20>>      locks(hw.buttons[0]);  // 20 s
alchemy::ParamLock<12, LockLength<30, 20>>  locks(hw.buttons[0], pager);
```

The second template argument is a capacity policy:
`LockLength<seconds, rate_hz = 30, store = LockStore::Preset>`.

---

## The cost model

```
samples per slot = seconds × rate_hz

RAM          = slots × samples × 2 B
preset bytes = slots × (7 + samples × 2 B)
```

(The 7 is the per-slot header: active flag, length, record base, and the
Clocked loop length in clock ticks.)

Both are compile-time constants you can read back, print at boot, or
assert against:

```cpp
using Locks = alchemy::ParamLock<6, alchemy::LockLength<20>>;

Locks::kMaxSeconds       // 20
Locks::kSampleRateHz     // 30
Locks::kFramesPerSlot    // 600
Locks::kArenaBytes       // 7'200   — motion storage
Locks::RamBytes()        // 7'560   — arena + the object itself
Locks::kPresetBytes      // 7'242   — taken from every preset slot
Locks::kPresetBytesFree  // 13'206  — left for your other components
```

## The two budgets

**RAM.** Framework statics link into `DTCMRAM` — 128 KiB, shared with
`.data` and the stack. ParamLock will not silently take a large bite out
of that: past **16 KiB** of motion arena it refuses to hold its own
storage and makes you place it (see *Large configurations* below). Raise
the threshold with `-DALCHEMY_LOCK_INLINE_RAM_LIMIT=32768` if your
application genuinely has the DTCM to spare.

**Flash.** A preset slot holds **20,448 bytes** for *all* managed
components combined. This ceiling is hard: the preset region is the top
640 KiB of the QSPI chip, ending at the last byte of the device, so it
cannot grow without taking space from firmware or giving up preset slots.
A ParamLock configured past it is a compile error, not a `Save()` that
returns `false` on the bench.

## Picking a rate

`kRateHz` is the rate the motion buffer is *stored* at, decoupled from
the control-loop frame rate. Playback interpolates between stored
samples and recording box-averages into them, so a low rate costs
smoothness far more slowly than you would expect — a hand turning a knob
carries no useful energy above about 8 Hz.

| Rate | Use it when |
|---|---|
| 60 Hz | You want the buffer indistinguishable from the control loop. Costs 2× of 30. |
| 30 Hz | The default. Smooth for anything a hand can do. |
| 20 Hz | Still fine, and what makes the longest configurations fit in flash. |

**Dropping the rate is almost always cheaper than dropping the length.**

## What fits in a preset slot

Assuming ~1 KiB for your other managed components (Pager, Settings,
preset names):

| Configuration | 6 slots | 12 slots | 16 slots |
|---|---|---|---|
| 16 s @ 30 Hz (default) | 5.8 KB ✅ | 11.6 KB ✅ | 15.5 KB ✅ |
| 20 s @ 30 Hz | 7.2 KB ✅ | 14.5 KB ✅ | 19.3 KB ✅ |
| 30 s @ 30 Hz | 10.8 KB ✅ | 21.7 KB ❌ | ❌ |
| 30 s @ 20 Hz | 7.2 KB ✅ | 14.5 KB ✅ | 19.3 KB ✅ |
| 30 s @ 60 Hz | 21.6 KB ❌ | ❌ | ❌ |

`LockStore::RamOnly` removes the flash ceiling entirely — see below.

## Locks you don't need to save

If you want long locks and don't need them to survive a power cycle,
say so and the flash budget stops applying:

```cpp
using Locks = alchemy::ParamLock<6,
    alchemy::LockLength<60, 60, alchemy::LockStore::RamOnly>>;
```

`SerializedSize()` becomes 0, so `presets.Manage(locks)` costs nothing
and the preset budget ignores them entirely. The raw byte form is still
available by hand via `Save()` / `Restore()` — that's the route for
writing locks to an SD card yourself.

## Large configurations

Past `ALCHEMY_LOCK_INLINE_RAM_LIMIT` the arena has to be placed by you.
The build tells you so, and this is the shape it's asking for:

```cpp
using Locks = alchemy::ParamLock<12,
    alchemy::LockLength<60, 60, alchemy::LockStore::RamOnly>>;

static uint16_t ALCHEMY_SDRAM_BSS lock_arena[Locks::kArenaSamples];
static Locks locks(hw.buttons[0], pager,
                   alchemy::LockArena{lock_arena, sizeof lock_arena});

int main() {
    hw.Init();       // brings up the FMC — SDRAM is unusable before this
    locks.Init();    // clears the arena; .sdram_bss is NOLOAD
    ...
}
```

Two rules:

1. **The arena may live in SDRAM; the ParamLock object may not.** The
   arena is plain `uint16_t` storage with no constructor, so it is safe
   in a NOLOAD section that static init never touches. The ParamLock
   object's constructor runs *before* `hw.Init()`, so it must stay in
   ordinary `.bss`.
2. **Call `locks.Init()` from `main()`, after `hw.Init()`.** `.sdram_bss`
   is NOLOAD.

A short arena is refused at runtime rather than overrun: `IsReady()`
stays false and every read returns 0.

## Seconds mean seconds

The motion buffer is clocked in milliseconds, not frames. `ControlLoop`
tells the surface its frame interval every frame, so:

- `LockLength<20>` is twenty seconds at 16 ms frames *and* at 32 ms frames.
- Changing `ControlLoop::FrameMs` does not rescale existing recordings.
- A lock replays at the speed it was recorded at.

If you drive `ParamLock` yourself instead of through `ControlLoop`, call
`SetFrameMs()` once at startup when your loop isn't running at the 16 ms
default.

## Changing a length later

`SchemaHash()` folds in the slot count, the buffer length, the sample
rate, and the storage policy. Changing any of them makes existing preset
slots **invisible** — they are treated as empty rather than replayed at
the wrong speed or restored into a differently-shaped buffer.

The v0.11 slot header grew from 5 to 7 bytes (`'PLK1'` → `'PLK2'`, adding
the Clocked loop length), so locks saved by earlier firmware are invisible
to v0.11 builds in the same deliberate way.

## Sample representation

Samples are stored as `uint16` normalized 0..1. Pot positions arrive
from a 16-bit ADC, so a 1/65535 grid is finer than the signal and three
times finer than `kCatchTolerance`; storing `float` would double both
budgets to represent noise.

---

# Behavior

Everything below is optional.  The declaration alone gives you the stock
behavior: arm at a 0.5% nudge, clear at 3%, Latch exit, a solid red pip
while recording, a blinking red pip during playback, free-running loops.
Each fluent call overrides one thing; defaults are shown.

```cpp
locks.UseClock(clock_)                              /* enables Clocked mode  */
     .SnapGrid(LockGrid::Straight | LockGrid::Triplet)  /* | LockGrid::Dotted */
     .ArmThreshold(0.005f)
     .ClearThreshold(0.03f)
     .ExitMode(LockExit::Latch)                     /* or LockExit::Return   */
     .RecordStyle({0xFF, 0x00, 0x00}, LockAnim::Solid)
     .PlayStyle  ({0xFF, 0x00, 0x00}, LockAnim::Blink);
```

## The gesture, and why there are two thresholds

Hold the trigger and move a knob: recording starts the moment the motion
passes `ArmThreshold`.  The distance to arm is timing error on the loop's
t = 0, so it is small — 0.5% of travel, roughly 1.5° on a 300° pot,
still above the filtered pot noise floor (the baseline snapshots at
button-down, so noise cannot creep toward the threshold during a hold).
The recording's reference (and a `Return` exit's home) is where the knob
sat **before** the nudge, so the loop is anchored to the gesture's true
origin, not origin-plus-threshold.

## Exit modes

What happens to the knob's base value when the button is released:

- **`LockExit::Latch`** (default) — the pot stays where the gesture left
  it, and playback rides on top as an offset.
- **`LockExit::Return`** — the logical value snaps back to the recorded
  origin and pot-catch re-arms, exactly as if you had switched pages: the
  pot is dead until it passes back through the stored value.  Playback
  reproduces what you recorded, verbatim.  Requires the paged form —
  there has to be a Pager to write the value into — and applies to every
  slot the hold armed, buffer-full auto-arms included.

## Clocked mode

`UseClock(clock_)` hands the surface a `MusicalClock` (internally timed
or steered by a `ClockFollower` — the lock does not care).  With the mode
set to **Clocked**, a finished recording snaps its *loop boundary* to the
nearest musical duration: play roughly a bar of motion and the loop
restarts on exactly the bar.  Three properties are worth being precise
about:

- **The motion is sacred.**  Playback always runs at exactly the speed
  you performed. A take
  slightly longer than the grid gets its tail trimmed (those samples are
  simply never reached); a slightly shorter one holds its final value
  until the restart.  At a stable tempo both corrections are the few
  milliseconds you missed the grid by.
- **Only the boundary snaps, and only the boundary follows tempo.**  The
  loop's start is wherever your gesture ended — deliberately not
  quantized to the bar.  Restarts are derived from the clock's absolute
  position (nothing accumulates, nothing drifts), so tempo changes and
  follower drift move the restart, never the motion speed.  Slow the
  clock way down and the motion plays out, holds, and restarts on the
  later grid; speed it way up and each pass replays the front of the
  take.
- **A stopped transport holds clocked loops in place** (free loops keep
  running); signal-loss behavior is whatever your `ClockFollower` loss
  policy says.

The take's duration is measured against the clock itself — a tick stamp
at arm, a tick delta at release — never derived from the sample count.
The clock runs on real timestamps, so the snap is immune to control-loop
frame-timing error and correctly integrates a tempo change made while
you were still recording.

The snap grid is ratio-nearest (log-domain) across the enabled note-value
families — straight and triplet by default, dotted opt-in via
`SnapGrid()` — from a 16th note up to a whole note, plus **integer bar
counts** above one bar regardless of the mask, so a sloppy 5½-bar take
lands on 5 or 6 bars instead of stretching to 4 or 8.

The mode is consulted at record time only.  Every slot stores which
timing it was captured under (`musical_ticks` in the saved header), so
flipping the setting never re-times an existing lock, and a preset can
mix free and clocked slots.  Two more consequences of storing musical
ticks rather than milliseconds:

- **Presets stay on the grid at any tempo.**  A lock saved as 2 bars
  restarts every 2 bars at any later tempo — the motion inside plays as
  recorded.
- **Restored locks come back aligned.**  Slots loaded from a preset
  anchor at the clock's origin, so equal musical lengths are mutually
  phase-locked.

If Clocked is selected but the clock is not running when a recording
ends — or no clock was ever wired — that slot falls back to free and
replays at its recorded length.  Nothing refuses to record.

## The settings selector

The Free/Clocked choice is a user decision, so it lives in the Settings
menu:

```cpp
settings.UseBrightness();          /* P1 (as before)                   */
settings.UseLocks(locks);          /* P2: Free / Clocked selector      */
settings.UsePresets(presets);      /* P3–P4 (as before)                */
```

`UseLocks` is an ordinary persisted selector — one byte in the Settings
blob, saved and loaded with presets exactly like brightness, editable
from hosts as a normal enum field — that pushes its value into the lock
whenever it changes or loads.  It defaults to page 0, pot 1; relocate it
with `.At(page, pot)`, restyle with `.Colors(...)`, and set the boot
default with `.Default(LockSync::Clocked)`.

Call it after `UseClock` — offering "Clocked" on a module with no clock
wired is a debug-build assert, not a silent lie.

## LED styles

The stock overlay is a pip at 6 o'clock on any ring whose slot is busy.
`RecordStyle` / `PlayStyle` set its color and animation:

| `LockAnim` | Reads as |
|---|---|
| `Solid` | lock exists |
| `Blink` | 2 Hz square — the playback default |
| `Breathe` | slow ~2 s breath |
| `LoopPulse` | a flash at every loop restart, decaying over the loop's first fifth — loop length at a glance (solid while recording) |
| `Sweep` | the pip orbits the ring with the play head; while recording it crawls with the write head, i.e. buffer use |

There are no tuning knobs on the animations on purpose.  When the
curated set isn't enough, skip the stock `Render()` and compose your own
ring from `IsActive()` / `IsRecording()` / `PlayPhase()` — that has been
the escape hatch all along.

## Utilities

```cpp
locks.ClearSlot(pot);                /* visible page                    */
locks.ClearSlotAtPage(page, pot);    /* explicit slot                   */
locks.Freeze(true);                  /* hold every play head …         */
locks.Freeze(false);                 /* … resume where they held        */
locks.PlayPhase(pot);                /* 0..1 loop phase, for custom UIs */
```

`Freeze` holds playback only — recording is unaffected — and clocked
loops resume from where they froze rather than jumping to wherever the
clock got to.  Wire it to a button for the build-and-drop trick.