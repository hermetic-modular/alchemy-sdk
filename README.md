# alchemy-sdk

The Alchemy SDK is two code surfaces:

1. **A minimal hardware BSP (board support package)** for every Hermetic
   Modular hardware release.  Pin definitions, LED chain layout, and
   low-level drivers, conforming to the standards set out by the
   Electrosmith Daisy ecosystem.  Build from primitives and have full
   access to the interface board and Daisy DSP board at the core.

2. **A set of utilities under Framework** that unlock advanced features
   on Hermetic Modular hardware.  Declaratively leverage LED animations,
   field-programmable jacks, and conventions like Pot Catch, Parameter
   Lock, and settings pages — the same UX you would find on Hermetic
   Modular's formal hardware releases like [Echoa](https://hermeticmodular.com/modules/alchemy-lab/echoa) and
   [Spagyros](https://hermeticmodular.com/modules/alchemy-lab/spagyros).

Either build from scratch with the hardware BSP, or bring your own DSP
and get powerful out-of-the-box features with the Alchemy Framework, or
mix and match as you desire.

Supported boards:

- Hermetic Modular [Alchemy Lab](https://hermeticmodular.com/modules/alchemy-lab)
- Hermetic Modular [Azoth](https://hermeticmodular.com/modules/azoth) — coming soon

<img src="media/hero.jpg" alt="Hero Hardware" width="600">

## Beta notice and Community discussion

The Alchemy SDK is in beta.  APIs, surface names, and on-disk preset
formats may change before the first stable release. I (Luke at Hermetic Modular) look forward to your feedback, and want to make this SDK the best possible for your needs. Please file issues on GitHub or [Discord](https://discord.gg/FEEEQFdd2G).

## Quickstart

[`stereo_eq.cpp`](examples/stereo_eq/stereo_eq.cpp) is an excellent
place to start: it opts into most Alchemy Framework features,
demonstrating how to build a fully featured module by bringing your own DSP.

See [`alchemy-template`](https://github.com/hermetic-modular/alchemy-template)
for a project skeleton that vendors the Alchemy SDK and libDaisy as git
submodules.  This is the recommended starting point for your own module
firmware.

<details>
<summary>Installation and Requirements</summary>

These are the instructions for this SDK repo, and deploying the examples here. In a typical application of the SDK, you would vendor it as a git submodule, like you would with libDaisy.

### Requirements

- `git`
- `cmake` ≥ 3.21
- `ninja`
- `arm-none-eabi-gcc` ≥ 12
- `dfu-util`

#### Install the toolchain

Ubuntu / Debian:

```sh
sudo apt install git cmake ninja-build gcc-arm-none-eabi dfu-util
```

macOS (Homebrew):

```sh
brew install git cmake ninja dfu-util
brew install --cask gcc-arm-embedded
```

### Clone and build

```sh
git clone --recurse-submodules https://github.com/hermetic-modular/alchemy-sdk.git
cd alchemy-sdk
cmake --preset arm
cmake --build --preset arm
```

### Flash an example

Connect the front USB-C port and put the module in update mode: press
**B3** during the ~2 s bootloader window after power-on (rings
spin a warm-white comet), or simply hold while powering on.
The rings switch to a slow breathe and the
module stays in DFU mode until it's flashed or reset. Then:

```sh
cmake --build --preset arm --target stereo_eq-flash
```

Flashing also works without the button press if `dfu-util` starts
inside the 2 s window.

Other examples flash the same way, e.g. `kick-flash`, `clock_sync-flash`,
`v2_cal_test-flash`, `v2_cv_demo-flash`, `cv_playground-flash`.

</details>

## Working with CV and Jacks

There are ten jacks total. The middle six are field programmable to be CV in or out. The other four are the audio codec stereo in and out. These can be repurposed for CV in and out, with some caveats.

See [`cv_playground.cpp`](examples/cv_playground/cv_playground.cpp) for all of these in practice.

**J1, J2** are codec audio inputs, AC-coupled. AC coupling blocks DC so absolute voltage can't be read, but rising edges pass cleanly, allowing for clocks and triggers. Use `RisingEdge()`. A use case may be you have a module that does not process input audio, in which case you could use these jacks for trigger inputs, to free up the other switchable jacks. Theoretically you could also set a low trigger threshold and get gate signals up to the length it takes the AC cap to debias the DC signal - some testing is required to see how long these gates could be.

**J3–J8** are the field programmable jacks. They can be mode changed with `EnableCvOutput()` which closes an analog switch to route the backing DAC (MCP4728 on J3–J6, STM DAC1 on J7–J8); `DisableCvOutput()` disconnects the DAC. This can be set at boot or changed live for unique firmware development opportunities.

**J9, J10** are codec audio outputs, DC-coupled. `EnableCvOutput()` claims that codec channel from your audio callback and fills it with the `SetVolts()` target every block — the highest precision on the board (24-bit), at audio-block update rate.

### Jack reference

| Jack | Type | Output bits | Output latency | Input bits | Input latency | How to change |
|------|------|-------------|----------------|------------|---------------|---------------|
| J1 | Codec In | N/A | N/A | 1 | audio block | `SetTriggerThreshold(v)` |
| J2 | Codec In | N/A | N/A | 1 | audio block | `SetTriggerThreshold(v)` |
| J3 | Ext Dac | 12 | ~70 µs (I²C) | 16 | audio rate | `Enable/DisableCvOutput()` |
| J4 | Ext Dac | 12 | ~70 µs (I²C) | 16 | audio rate | `Enable/DisableCvOutput()` |
| J5 | Ext Dac | 12 | ~70 µs (I²C) | 16 | audio rate | `Enable/DisableCvOutput()` |
| J6 | Ext Dac | 12 | ~70 µs (I²C) | 16 | audio rate | `Enable/DisableCvOutput()` |
| J7 | STM Dac | 12 | <1 µs | 16 | audio rate | `Enable/DisableCvOutput()` |
| J8 | STM Dac | 12 | <1 µs | 16 | audio rate | `Enable/DisableCvOutput()` |
| J9 | Codec Out | 24 | audio block | N/A | N/A | `Enable/DisableCvOutput()` |
| J10 | Codec Out | 24 | audio block | N/A | N/A | `Enable/DisableCvOutput()` |

All output ranges are ±5 V at the panel.

## Bootloader Information

Alchemy Lab V2 boards run a board-specific fork of the Daisy bootloader. It serves DFU on the front-panel
USB-C port, renders bootloader state on the panel, and latches into update mode
when B3 is pressed during the boot window. 

<details>
<summary>Bootloader Details</summary>

[`The bootloader`](https://github.com/hermetic-modular/alchemy-lab-bootloader) is also open source.

Apps built with this SDK can also enter update mode programmatically:

```cpp
daisy::System::ResetToBootloader(
    daisy::System::BootloaderMode::DAISY_INFINITE_TIMEOUT);
```

This could be used to support multiple firmwares in flash at once without requiring a reflash as a future feature.

</details>

## Animation library

A ring rendering is a stack — one Base showing the value, plus any number
of overlays composed on a `RingFrame`, driven by plain floats (knob
values, published DSP state, time phases). Two overlay primitives cover
every animation; everything else is a parameter.

| Slot | Options |
|---|---|
| Base (exactly 1) | `Fill/Edge` · `Fill/Center` (movable pivot) · `Selector` · `Gradient` / `GradientFill` |
| Overlays (0..N) | `Pip` — a positioned element: cursor, comet, ping, playhead, landmark, notch, afterglow · `Field` — a brightness texture: breathe, ripple, shimmer, stutter, banding |
| Stateful animators | `Sparkle` · `BeatPip` |
| System | catch pip, painted by `Emit`, always on top |

Declarative rings (`ParamSlot` + `PerfRenderer`) and fully custom rings
share the same rendering core. See
[docs/ring-animations.md](docs/ring-animations.md) for the model, the
full parameter reference, and migration notes, and
[`examples/ring_demo`](examples/ring_demo) for a working composition.

## Calibration

Every production board self-calibrates its six CV jacks — DAC out and ADC in —
with no external equipment, using the board's built-in DAC→jack→ADC
loopback and the STM32's factory voltage reference.

**To calibrate:** unpatch all CV jacks, then hold **B1 + B2** while the
board resets.  The LED panel narrates the ~15 s procedure (rings fill
as each jack is swept), flashes green on success, and the board reboots
calibrated.  The result is a 120-byte record in a dedicated QSPI sector
that **survives firmware reflashes** — calibrate once per board, every
SDK firmware picks it up automatically.

The set volts functions apply calibration transparently - `hw.j3.SetVolts(volts)` and
`hw.j3.Volts()` - and fall back when no record exists. Use `hw.IsCalibrated()` to find out if a record exists.  See
[`v2_calibration.h`](hardware/alchemy-lab/v2/include/alchemy/hw/v2_calibration.h) for details and
[`examples/v2_cal_test`](examples/v2_cal_test/v2_cal_test.cpp) for a toggle calibration test app.

## Expansion Header

There is an expansion header on the back of the unit. In the future,
there will be official Hermetic Modular expanders. In the meantime, an
enterprising developer is welcome to develop their own expander modules.

<img src="media/header-pinout.png" alt="Header Render" width="600">

### Header Pinout

Numbered/named pins (`B1`–`B8`) are direct connections to the exposed
Daisy Seed2 DFM pins, and together expose **SPI1**, an **I²C** bus, and
**USART1 RX/TX**.

| Pin | Signal | &nbsp; | Pin | Signal |
| --: | :----- | :----: | --: | :----- |
|  11 | +12 V  |        |   1 | −12 V  |
|  12 | GND    |        |   2 | GND    |
|  13 | GND    |        |   3 | B2     |
|  14 | GND    |        |   4 | B4     |
|  15 | 3V3A   |        |   5 | GND    |
|  16 | GND    |        |   6 | B6     |
|  17 | GND    |        |   7 | B5     |
|  18 | GND    |        |   8 | B3     |
|  19 | GND    |        |   9 | B1     |
|  20 | B8     |        |  10 | B7     |

## Board versions

V2 is the board every firmware targets:

```sh
cmake --preset arm      # V2 (the default)
```

(Equivalent to setting `-DALCHEMY_BOARD=v2` by hand.  The legacy v1
board builds only with an explicit `-DALCHEMY_BOARD=v1`, and warns.)

- **v1** — original dev board. You probably don't have one of these unless you were an alpha tester.
- **v2** — The standard production board shipped by Hermetic Modular.

V2-only examples (`v2_cal_test`, `v2_cv_demo`, `cv_playground`) build only when
`ALCHEMY_BOARD=v2`. V1 doesn't have CV out.

## License

The Alchemy SDK is released under the [MIT License](LICENSE) — build, fork,
remix, and ship commercial firmware modules freely.

Bundled dependencies retain their own licenses; notably,
[libDaisy](vendor/libDaisy) is independently MIT-licensed by Electrosmith. We love Electrosmith!
