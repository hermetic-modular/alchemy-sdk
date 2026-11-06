#include "alchemy/midi.h"
#include "alchemy/midi/clock.h"
#include "alchemy/midi/daisy_transport.h"
#include "alchemy/midi/pager_target.h"

using namespace alchemy::midi;

template class NotePart<6, 32, 32, 2>;
template class ParameterMixer<32, 64>;
template class ControllerDecoder<32>;
template class Routes<32>;
template class RouteCodec<32>;
template class RoutesSerializable<32>;
template class TakeoverTracker<32>;
template class Ingress<256, 4>;
template class Output<256>;
template class Receiver<256>;
template class DaisyUartPort<256>;
template class DaisyUsbPort<256>;
template class PagerTargets<32>;
template class SnapshotExchange<PartConfig>;

void CompileMidi(NotePart<6, 32, 32, 2>& part, ControllerDecoder<32>& controllers,
                 Routes<32>& routes, ParameterMixer<32, 64>& parameters,
                 DaisyUartPort<256>& uart, DaisyUsbPort<256>& usb,
                 Dispatcher<4, 64>& dispatcher, ActiveSensing<4>& sensing,
                 Ingress<256, 4>& ingress, ControlEdits<32>& edits, Thru<4>& thru,
                 SnapshotExchange<PartConfig>& snapshots, ClockAdapter& clock,
                 const Event& event, uint32_t now) {
    part.Process(event, [](const NoteAction&) {});
    controllers.Process(event, [&](const ControllerMessage& message) {
        routes.Process(message, [&](const ParameterCommand& command) { parameters.Apply(command); });
    });
    dispatcher.Subscribe(CriticalSink{&ingress, [](void* context, const Event& e) {
        return static_cast<Ingress<256, 4>*>(context)->Push(e);
    }});
    const auto receive = [&](const Event& e) { return dispatcher.Accept(e); };
    if (dispatcher.Failed(uart.Source())) uart.MarkLost();
    if (dispatcher.Failed(usb.Source())) usb.MarkLost();
    const alchemy::TickTimebase::Sample sample{now, now, 1000000u, 0u};
    uart.Poll(sample, receive); usb.Poll(sample, receive); dispatcher.Poll(now);
    sensing.Observe(event); sensing.Poll(now, receive);
    ingress.Push(event); ingress.DrainDue(now, [](const Event&) {});
    edits.Drain(1, [](ParamId) { return 1u; }, [](const ParameterCommand&) {});
    thru.Process(event, [](uint16_t, const Event&) { return true; });
    snapshots.Publish(part.Config(), [](const PartConfig& c) { return NotePart<6, 32, 32, 2>::Validate(c); });
    SnapshotExchange<PartConfig>::Lease lease;
    snapshots.Acquire(lease);
    clock.Process(event); clock.Poll(now);
}
