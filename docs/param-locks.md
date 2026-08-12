# Parameter locks: length, rate, and what they cost

A parameter lock records a knob gesture and loops it. How long a gesture
can be is your application's decision, declared once at the ParamLock
declaration:

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
preset bytes = slots × (5 + samples × 2 B)
```

Both are compile-time constants you can read back, print at boot, or
assert against:

```cpp
using Locks = alchemy::ParamLock<6, alchemy::LockLength<20>>;

Locks::kMaxSeconds       // 20
Locks::kSampleRateHz     // 30
Locks::kFramesPerSlot    // 600
Locks::kArenaBytes       // 7'200   — motion storage
Locks::RamBytes()        // 7'440   — arena + the object itself
Locks::kPresetBytes      // 7'230   — taken from every preset slot
Locks::kPresetBytesFree  // 13'218  — left for your other components
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
| 16 s @ 30 Hz (default) | 5.8 KB ✅ | 11.6 KB ✅ | 15.4 KB ✅ |
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

## Sample representation

Samples are stored as `uint16` normalized 0..1. Pot positions arrive
from a 16-bit ADC, so a 1/65535 grid is finer than the signal and three
times finer than `kCatchTolerance`; storing `float` would double both
budgets to represent noise.