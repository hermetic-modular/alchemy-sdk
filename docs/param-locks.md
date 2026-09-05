# Parameter locks: capacity, rate, and behavior

A parameter lock records a knob gesture and loops it. Recording arms
when the knob moves 0.5% of travel. A 3% move on a knob that is already
looping clears it. An optional **Clocked** mode restarts each loop on
the musical grid. Capacity is a compile-time declaration.

Maximum gesture length is declared once, as a template argument:

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
Locks::kArenaBytes       // 7'200   (motion storage)
Locks::RamBytes()        // 7'560   (arena + the object itself)
Locks::kPresetBytes      // 7'242   (taken from every preset slot)
Locks::kPresetBytesFree  // 13'206  (left for your other components)
```

## The two budgets

**RAM.** Framework statics link into `DTCMRAM` (128 KiB, shared with
`.data` and the stack). Above **16 KiB** of motion arena the build fails
unless you supply the arena yourself (see *Large configurations*). Raise
the threshold with `-DALCHEMY_LOCK_INLINE_RAM_LIMIT=32768` if the DTCM
is available.

**Flash.** A preset slot holds **20,448 bytes** for *all* managed
components combined. The preset region is the top 640 KiB of the QSPI
chip and cannot grow without taking space from firmware or dropping
preset slots. A ParamLock configured past the ceiling is a compile
error, not a runtime `Save()` failure.

## Picking a rate

`kRateHz` is the rate the motion buffer is *stored* at, decoupled from
the control-loop frame rate. Playback interpolates between stored
samples and recording box-averages into them. Hand motion on a knob has
negligible energy above about 8 Hz, so low rates lose little smoothness.

| Rate | Use it when |
|---|---|
| 60 Hz | Near the 16 ms control-loop rate. Twice the cost of 30 Hz. |
| 30 Hz | Default. Sufficient for hand motion. |
| 20 Hz | Sufficient. Required for the longest configurations to fit in flash. |

**Reduce the rate before reducing the length.**

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

`LockStore::RamOnly` removes the flash ceiling entirely (see *RAM-only
locks*).

## RAM-only locks

Locks that need not survive a power cycle can be declared RAM-only,
which removes them from the flash budget:

```cpp
using Locks = alchemy::ParamLock<6,
    alchemy::LockLength<60, 60, alchemy::LockStore::RamOnly>>;
```

`SerializedSize()` returns 0, so `presets.Manage(locks)` adds nothing to
the preset budget. `Save()` / `Restore()` still produce the raw byte
form, for example for writing locks to an SD card.

## Large configurations

Above `ALCHEMY_LOCK_INLINE_RAM_LIMIT` you must supply the arena; the
build fails with a message saying so. Required form:

```cpp
using Locks = alchemy::ParamLock<12,
    alchemy::LockLength<60, 60, alchemy::LockStore::RamOnly>>;

static uint16_t ALCHEMY_SDRAM_BSS lock_arena[Locks::kArenaSamples];
static Locks locks(hw.buttons[0], pager,
                   alchemy::LockArena{lock_arena, sizeof lock_arena});

int main() {
    hw.Init();       // brings up the FMC; SDRAM is unusable before this
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

## Length is wall-clock time

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
slots **invisible**: they are treated as empty rather than replayed at
the wrong speed or restored into a differently-shaped buffer.

The v0.11 slot header grew from 5 to 7 bytes (`'PLK1'` → `'PLK2'`, adding
the Clocked loop length), so v0.11 builds treat locks saved by earlier
firmware as empty.

## Sample representation

Samples are stored as `uint16` normalized 0..1. Pot positions come from
a 16-bit ADC, so a 1/65535 grid matches the source resolution. `float`
would double both budgets without adding information.

---

# Behavior

Everything below is optional. The declaration alone gives: arm at 0.5%,
clear at 3%, Latch exit, solid red pip while recording, blinking red pip
during playback, free-running loops. Each call overrides one setting;
the values shown are the defaults.

```cpp
locks.UseClock(clock_)                              /* enables Clocked mode  */
     .SnapGrid(LockGrid::Straight | LockGrid::Triplet)  /* | LockGrid::Dotted */
     .ArmThreshold(0.005f)
     .ClearThreshold(0.03f)
     .ExitMode(LockExit::Latch)                     /* or LockExit::Return   */
     .RecordStyle({0xFF, 0x00, 0x00}, LockAnim::Solid)
     .PlayStyle  ({0xFF, 0x00, 0x00}, LockAnim::Blink);
```

## Arm and clear thresholds

Hold the trigger and move a knob. Recording starts when the motion
exceeds `ArmThreshold`. Travel before that point is lost from the start
of the loop, so the threshold is kept small: 0.5% of travel, about 1.5°
on a 300° pot, and about 3× the filtered pot noise floor. The baseline
is captured at button-down, so noise cannot accumulate toward the
threshold during a hold. The recording's reference value, and the home
position for a `Return` exit, is the knob position before the nudge, not
the position at which the threshold was crossed.

`ClearThreshold` is larger (3%) so that brushing a neighboring pot
during a hold does not clear a playing lock.

## Exit modes

What happens to the knob's base value when the button is released:

- **`LockExit::Latch`** (surface default): the pot stays where the
  gesture left it, and playback is added to it as an offset.
- **`LockExit::Return`**: the logical value returns to the recorded
  origin and pot-catch re-arms, as on a page change. The pot has no
  effect until it crosses the stored value. Playback reproduces the
  recording exactly. Requires the paged form, since the value is written
  into a Pager. Applies to every slot the hold armed, including
  buffer-full auto-arms.

`UseLocks` also exposes the exit policy as a persisted Settings selector
(see *The settings selectors*). The two defaults differ: the surface
default is Latch, the selector default is Return.

## Clocked mode

`UseClock(clock_)` supplies a `MusicalClock`. Whether it is internally
timed or driven by a `ClockFollower` makes no difference to the lock. In
**Clocked** mode a finished recording snaps its *loop boundary* to the
nearest musical duration: record roughly one bar of motion and the loop
restarts on exactly one bar. Three properties:

- **Motion is never resampled.** Playback runs at the recorded speed. A
  take longer than the snapped length is truncated: samples past the
  boundary are never reached. A shorter take holds its final value until
  the restart. At stable tempo either correction is the few milliseconds
  by which the take missed the grid.
- **Only the boundary snaps and follows tempo.** The loop starts where
  the gesture ended; the start is not quantized to the bar. Restart
  times are computed from the clock's absolute position rather than
  accumulated, so tempo changes and follower drift move the restart and
  never the motion speed. If the clock slows, the motion plays out,
  holds its final value, and restarts on the later grid. If the clock
  speeds up, each pass replays only the front of the take.
- **A stopped transport freezes clocked loops.** Free loops keep
  running. On signal loss, behavior follows the `ClockFollower` loss
  policy.

The take's duration is measured in clock ticks (a stamp at arm, a delta
at release), not from the sample count. The clock uses real timestamps,
so the snap is unaffected by control-loop frame jitter and accounts for
a tempo change made during recording.

The snap grid is nearest-by-ratio (log domain) across the enabled
note-value families: straight and triplet by default, dotted via
`SnapGrid()`. It spans a 16th note to a whole note, plus **integer bar
counts** above one bar regardless of the mask, so a 5½-bar take snaps to
5 or 6 bars rather than 4 or 8.

The mode is consulted at record time only. Every slot stores which
timing it was captured under (`musical_ticks` in the saved header), so
changing the setting never re-times an existing lock, and a preset can
mix free and clocked slots. Two more consequences of storing musical
ticks rather than milliseconds:

- **Presets stay on the grid at any tempo.** A lock saved as 2 bars
  restarts every 2 bars at any later tempo; the motion plays as
  recorded.
- **Restored locks are phase-aligned.** Slots loaded from a preset
  anchor at the clock's origin, so slots of equal musical length restart
  together.

If Clocked is selected but the clock is not running when a recording
ends, or no clock was ever wired, that slot falls back to free and
replays at its recorded length. Recording is never refused.

## The settings selectors

The Free/Clocked choice and the exit policy are exposed in the Settings
menu:

```cpp
settings.UseBrightness();          /* P1                               */
settings.UsePresets(presets);      /* P3–P4                            */
settings.UseLocks(locks);          /* P5: Free/Clocked, P6: exit       */
```

`UseLocks` installs two persisted selectors. Each is one byte in the
Settings blob, saved and loaded with presets like brightness, and
editable from hosts as an enum field. Each pushes its value into the
lock on change and on load. P5 selects Free or Clocked for new
recordings. P6 selects the exit policy: **Return** (zone 0, default) or
**Latch** (zone 1); see *Exit modes*. P2 is unassigned. Default
assignment: P1 brightness, P2 free, P3–P4 presets, P5–P6 locks.

Relocate the pair with `.At(page, pot)` / `.ExitAt(page, pot)`, restyle
the sync zones with `.Colors(...)`, and set boot defaults with
`.Default(LockSync::Clocked)` / `.DefaultExit(LockExit::Latch)`.

Call `UseLocks` after `UseClock`: offering Clocked with no clock wired
is a debug-build assert. `UseLocks` requires the paged form, because
Return needs a Pager to write into.

## LED styles

The stock overlay is a pip at 6 o'clock on any ring whose slot is active.
`RecordStyle` / `PlayStyle` set its color and animation:

| `LockAnim` | Reads as |
|---|---|
| `Solid` | lock exists |
| `Blink` | 2 Hz square wave; playback default |
| `Breathe` | 2 s triangle brightness ramp |
| `LoopPulse` | flash at each loop restart, decaying over the first 20% of the loop; solid while recording |
| `Sweep` | pip position follows the play head around the ring; while recording it follows the write head, showing buffer fill |

The animations have no tuning parameters. For anything else, skip the
stock `Render()` and draw the ring yourself from `IsActive()` /
`IsRecording()` / `PlayPhase()`.

## Utilities

```cpp
locks.ClearSlot(pot);                /* visible page                    */
locks.ClearSlotAtPage(page, pot);    /* explicit slot                   */
locks.Freeze(true);                  /* hold every play head …         */
locks.Freeze(false);                 /* … resume where they held        */
locks.PlayPhase(pot);                /* 0..1 loop phase, for custom UIs */
```

`Freeze` affects playback only; recording continues. Clocked loops
resume from the frozen phase, not from the clock's current position.
Typically wired to a performance button.