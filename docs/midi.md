# MIDI integration

MIDI is optional: link `alchemy::midi_daisy` for transport and
`alchemy::midi_clock` when following clock. HostLink and MIDI share one composite
USB device; default startup works in either order. To choose the shared USB name
or VID/PID, call `usb::Device::Configure` before either service initializes.
An explicit conflicting product/identity is rejected. Do not also initialize
Daisy's standalone USB device service.

A USB instrument needs a port, an audio queue, and a note part. Call `InitMidi`
after hardware startup and before starting audio. Service `PollMidi` from the
foreground, including while settings are open. Call `DrainMidi` before rendering
each audio block and use the action sink to drive the voice.

```cpp
#include "alchemy/midi.h"
#include "alchemy/midi/usb_transport.h"

using namespace alchemy;
using namespace alchemy::midi;

static DaisyUsbPort<> midi_port;
static Ingress<128, 1> audio_input;
static NotePart<1, 32> notes;
static TickTimebase foreground_time, audio_time;

bool InitMidi() {
    const auto frequency = daisy::System::GetTickFreq();
    if (!frequency) return false;
    const auto epoch = daisy::System::GetTick();
    foreground_time.Init(frequency, epoch);
    audio_time.Init(frequency, epoch);
    if (!usb::Device::Shared().Configure(daisy::UsbHandle::FS_EXTERNAL, "Example")) return false;
    PartConfig config;
    config.listen = Listen::Omni();
    config.articulation = ArticulationPolicy::Legato();
    config.slide.trigger = SlideTrigger::KeyOverlap;
    return notes.Init(config) && midi_port.Init({}, {0, 1});
}

void PollMidi() {
    foreground_time.Update(daisy::System::GetTick());
    const auto now = foreground_time.GetSample();
    midi_port.Poll(now, [](const Event& e) { return audio_input.Push(e); });
}

template<class ActionSink>
void DrainMidi(ActionSink&& action_sink) {
    const auto now = audio_time.Update(daisy::System::GetTick());
    audio_input.DrainDue(now, [&](const Event& e) {
        notes.Process(e, action_sink);
    });
}
```

Each timebase has one owner. Starting both from the same hardware tick gives the
foreground and audio threads a shared time origin. `DrainDue` consumes events in
time order. Use a separate scheduler for future application events.

For multiple ports or subscribers, forward each port to `Dispatcher::Accept` and
call `Dispatcher::Poll` after servicing the ports. Subscribe an audio queue as a
`CriticalSink` and propagate `dispatcher.Failed(port.Source())` via `port.MarkLost()`
before polling that port. The dispatcher delivers the old source's reset before
events from its replacement.

To add clock following and a CC20 mapping, initialize the following components
with `InitControls` during startup. For each event in `DrainMidi`, also call
`ProcessControls(e, now)`. After draining the queue, call `midi_clock.Poll(now)`
once per block, including blocks without MIDI events. Read `parameters.Value(1)`
to get the modulated parameter.

```cpp
#include "alchemy/midi/clock.h"

static ControllerDecoder<16> controllers;
static Routes<1> routes;
static ParameterMixer<1, 16> parameters;
static MusicalClock musical_clock;
static ClockFollower follower(musical_clock);
static ClockAdapter midi_clock(follower, musical_clock);

bool InitControls() {
    Route route;
    route.id = route.target = 1;
    route.number = 20;
    route.listen = Listen::Omni();
    route.operation = RouteOp::AddOffset;
    route.minimum = -0.5f;
    route.maximum = 0.5f;
    route.bipolar = true;
    return parameters.Register({1, ParameterType::Continuous, 0, 1, 0.5f})
        && routes.Add(route) && routes.ValidateTargets(parameters);
}

void ProcessControls(const Event& e, uint32_t now) {
    if (!midi_clock.Enabled() || e.source != midi_clock.Source()) midi_clock.SelectSource(e.source);
    if (e.type == EventType::SystemReset) {
        controllers.Clear();
        parameters.ClearModulation();
    }
    midi_clock.Process(e, now);
    controllers.Process(e, [](const ControllerMessage& message) {
        if (message.kind == ControllerKind::Disconnect || message.kind == ControllerKind::Reset)
            parameters.ResetSource(message.source, message.channel);
        else routes.Process(message, [](const ParameterCommand& command) { parameters.Apply(command); });
    });
}
```

DSP actions carry `pitch_semitones` plus `expression.TotalBend()`. Apply independent
amp/filter attack flags, honor `pitch_transition` on Assign as well as Retarget,
and retain pitch history across envelope idle. Kill may request `retain_pitch`
for reassignment. Report `NotifyIdle` using the action's voice generation when a
release ends; paraphonic instruments additionally report `NotifyGroupIdle`.
Apply mono settings with `part.ApplyConfig(options.Part(), action_sink)`. Selecting
Off kills sounding voices and ignores channel input until a listen mode is selected.
Poly attack and onset policies apply per voice; mono fallback from a Note Off
does not count as a new attack.

The example has static configuration and additive CC lanes. For live settings or
preset changes, publish validated configuration with `SnapshotExchange`; the
audio owner applies it. Base-edit routes additionally use `ControlEdits` to
acknowledge resolved values to the panel with configuration/base revisions.
Reset mixer lanes and takeover state together on disconnect/controller reset;
clear all application/controller modulation for part-wide System Reset. Learn
and restored routes must pass `ValidateTargets` before publication.

`Output::Send`/`Schedule` allow one producer, including audio. `Output::Poll` and
direct transport sends/SysEx writes are foreground-only. `Accepted` means queued,
not delivered. Busy and transport Error retain the group; Invalid/Unsupported
cancel it. Begin a canceled SysEx stream with a new F0.

USB availability/recovery observes bus state during foreground service and USB
I/O. A complete suspend/resume between observations is not latched by the current
HAL glue. Pending transfers retire on observed suspension; already-started
transfers retain their backing buffers and may complete normally.
