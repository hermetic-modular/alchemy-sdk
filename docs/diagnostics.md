# USB diagnostics

Use HostLink for USB messages and live values. The SDK chooses the board's
USB connector, owns initialization, and shares the connection with presets.

## Add diagnostics to an existing Host

```cpp
#include "alchemy/host_link/diagnostics.h"

static alchemy::hostlink::Diagnostics debug;

// During setup, before the first host poll:
host.Extend(debug);
loop.Use(host); 

// After hw.Init(), on the control-loop thread:
debug.Info("Ready");
debug.Warn("Preset %u used factory defaults", unsigned(slot));
```

The extension has no separate `StartLog`, USB handle, or wait-for-PC call.
Messages emitted before the first poll are retained until the bounded ring
fills. Firmware continues running with no client connected.

Do not also call `hw.seed.StartLog()`, `hw.seed.PrintLine()`, or direct
USB transmit functions on the HostLink port. Replace those logging calls
with `debug.Info()` or the migration alias `debug.PrintLine()`.

## Firmware without presets

```cpp
static alchemy::hostlink::Diagnostics debug;
static alchemy::hostlink::Host host(
    "my_module", "My Module", "1.0.0", GIT_HASH);

// After hw.Init():
host.Extend(debug);
loop.Use(host);
debug.Info("Ready");
// for (;;) loop.Tick();
```

No dummy preset store is needed. Without a `ControlLoop`, call
`host.Poll(now_ms)` frequently from your own main loop. Include `host.h`
and link `alchemy::host_link`. See the complete
[`examples/diagnostics`](../examples/diagnostics/diagnostics.cpp) example.

## Live scalar values

```cpp
static alchemy::hostlink::Gauge<float> peak("audio.peak", "Output peak");
static alchemy::hostlink::Gauge<uint32_t> underruns("audio.underruns", "Underruns");

// Setup:
peak.Unit("FS");
debug.Watch(peak);
debug.Watch(underruns);

// Runtime, including an audio callback:
peak.Set(block_peak);
underruns.Set(underrun_count);
```

Supported types are `float`, `int32_t`, `uint32_t`, and `bool`. `Set()`
publishes one lock-free scalar with no formatting or USB work. A gauge
contains the latest value, not a history or an accumulated counter;
maintain counters in your application and publish their current value.
The client shows unset values as unavailable.

Register during setup. `Watch()` returns false for invalid metadata,
duplicate IDs, or capacity issues.
Check `debug.ConfigurationOk()` and `host.ConfigurationOk()` after setup.

## Messages and bounded cost

`Debug`, `Info`, `Warn`, `Error`, and `PrintLine` accept printf. 
`PrintLine` is an Info alias. `MinimumLevel(LogLevel::Warn)` can
reduce production logging.

Formatted logging is for the control-loop thread, never an interrupt or
audio callback. The SDK uses fixed log storage, never waits for USB, and
never saves logs to flash. Messages longer than the limit are truncated.

Default storage is 32 records with 160 text bytes each (roughly 6 KiB SRAM).
`ALCHEMY_DIAGNOSTIC_RECORDS` (1–1024) and `ALCHEMY_DIAGNOSTIC_TEXT` (16–240)
can change these bounds. Define them consistently for the SDK library and
all consumers. Without a Diagnostics instance there is no log-ring cost.
Messages are volatile and cannot be retrieved from a reset device or
while the application is halted; use SWD for breakpoints and hard faults.

## CLI

```sh
node tools/hostlink-cli/hostlink.mjs -p /dev/cu.usbmodem1234 logs --follow
node tools/hostlink-cli/hostlink.mjs -p /dev/cu.usbmodem1234 logs --level warn
node tools/hostlink-cli/hostlink.mjs -p /dev/cu.usbmodem1234 logs --json
node tools/hostlink-cli/hostlink.mjs -p /dev/cu.usbmodem1234 watch
```

`--json` emits one JSON object per line. `watch` displays live values.
An explicit port is recommended with multiple devices. The CLI supports.

Wire reference: [HostLink protocol §9](hostlink-protocol.md#9-diagnostics-commands-0x600x63).

## Web programmer

Open **Device console** beneath the programmer. 
