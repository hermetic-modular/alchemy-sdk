#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include "alchemy/midi/parameter.h"
#include "alchemy/midi/persistence.h"
#include "alchemy/midi/takeover.h"

using namespace alchemy::midi;

static void Near(float a, float b) { assert(std::fabs(a - b) < 0.00001f); }
static void FixCrc(uint8_t* bytes, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= i >= 12 && i < 16 ? 0 : bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    crc = ~crc;
    for (unsigned i = 0; i < 4; ++i) bytes[12 + i] = static_cast<uint8_t>(crc >> (8 * i));
}

int main() {
    ParameterMixer<1, 2> frequency;
    ParameterDescriptor cutoff;
    cutoff.id = 100; cutoff.minimum = 20.f; cutoff.maximum = 20000.f; cutoff.initial = 440.f;
    assert(frequency.Register(cutoff));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    assert(frequency.Value(100, nan, 0.f) == 440.f && frequency.Value(100, 0.f, inf) == 440.f);
    assert(frequency.SetBase(100, 880.f));
    assert(frequency.Value(100, -inf, 0.f) == 880.f && frequency.Value(100, 0.f, nan) == 880.f);
    assert(frequency.Value(100, -1000.f) == 20.f && frequency.Value(100, 30000.f) == 20000.f);
    assert(frequency.Value(999, nan, inf) == 0.f);
    ParameterMixer<4, 16> mixer;
    ParameterDescriptor descriptor;
    descriptor.id = 1; descriptor.initial = 0.5f;
    descriptor.operations |= OperationBit(RouteOp::Multiply);
    assert(mixer.Register(descriptor));
    assert(!mixer.Register(descriptor));
    ParameterCommand command;
    command.target = 1; command.route = 1; command.source = {1, 1};
    command.operation = RouteOp::AddOffset; command.value = 0.4f;
    assert(mixer.Apply(command)); Near(mixer.Value(1), 0.9f); Near(mixer.Base(1), 0.5f);
    command.value = -0.2f; assert(mixer.Apply(command)); Near(mixer.Value(1), 0.3f);
    command.source = {2, 1}; command.value = 0.1f;
    assert(mixer.Apply(command)); Near(mixer.Value(1), 0.6f);
    mixer.ResetSource({1, 1}); Near(mixer.Value(1), 0.6f);
    mixer.ResetSource({2, 1}); Near(mixer.Value(1), 0.5f);
    command.source = {1, 1}; command.reduction = Reduction::Sum; command.value = 0.2f;
    assert(mixer.Apply(command)); command.source = {2, 1}; command.value = -0.1f;
    assert(mixer.Apply(command)); Near(mixer.Value(1), 0.6f);
    mixer.ClearModulation();
    command.reduction = Reduction::Maximum;
    command.source = {1, 1}; command.value = 0.2f; assert(mixer.Apply(command));
    command.source = {2, 1}; command.value = 0.1f; assert(mixer.Apply(command));
    Near(mixer.Value(1), 0.7f); mixer.ResetSource({1, 1}); Near(mixer.Value(1), 0.6f);
    mixer.ClearModulation();
    command.operation = RouteOp::Overlay; command.source = {1, 1}; command.value = 0.2f;
    assert(mixer.Apply(command)); command.source = {2, 1}; command.value = 0.8f;
    assert(mixer.Apply(command)); Near(mixer.Value(1), 0.8f);
    mixer.ResetSource({2, 1}); Near(mixer.Value(1), 0.2f);
    mixer.RemoveRoute(1); Near(mixer.Value(1), 0.5f);
    command.operation = RouteOp::Multiply; command.value = 0.5f;
    assert(mixer.Apply(command)); Near(mixer.Value(1), 0.25f);
    mixer.RemoveRoute(1); Near(mixer.Value(1), 0.5f);
    Near(mixer.Value(1, 0.8f, -0.8f), 0.5f);
    command.operation = RouteOp::EditBase; command.value = 0.7f;
    assert(mixer.Apply(command)); Near(mixer.Base(1), 0.7f);
    command.operation = RouteOp::RelativeBase; command.value = 1.f;
    assert(mixer.Apply(command)); Near(mixer.Base(1), 1.f);
    command.value = -2.f; assert(mixer.Apply(command)); Near(mixer.Base(1), 0.f);
    command.value = std::numeric_limits<float>::quiet_NaN(); assert(!mixer.Apply(command));
    assert(!mixer.SetBase(1, command.value));
    assert(mixer.SetBase(1, 0.5f));
    command.value = 0.2f; command.operation = RouteOp::AddOffset; command.channel = 1;
    assert(mixer.Apply(command)); command.channel = 2; command.value = 0.3f;
    assert(mixer.Apply(command)); mixer.ResetSource(command.source, 1); Near(mixer.Value(1), 0.8f);
    mixer.ResetSource(command.source, 2); Near(mixer.Value(1), 0.5f);
    command.source = {2, 2}; command.channel = 0; assert(mixer.Apply(command));
    mixer.ResetSource({2, 1}); Near(mixer.Value(1), 0.8f);
    mixer.ResetSource({2, 2}); Near(mixer.Value(1), 0.5f);

    descriptor.id = 2; descriptor.type = ParameterType::Selector;
    descriptor.maximum = 4; descriptor.initial = 0;
    descriptor.operations = OperationBit(RouteOp::Select) | OperationBit(RouteOp::RelativeBase);
    assert(mixer.Register(descriptor));
    command.target = 2; command.operation = RouteOp::Select; command.value = 2.7f;
    assert(mixer.Apply(command)); Near(mixer.Value(2), 3.f);
    command.operation = RouteOp::Multiply; assert(!mixer.Apply(command));
    descriptor.id = 3; descriptor.type = ParameterType::Trigger; descriptor.maximum = 1;
    descriptor.operations = OperationBit(RouteOp::Trigger);
    assert(mixer.Register(descriptor));
    command.target = 3; command.operation = RouteOp::Trigger; command.trigger = TriggerEdge::Rising;
    command.value = 1.f; assert(mixer.Apply(command).triggered);
    assert(!mixer.Apply(command).triggered);
    command.value = 0.f; assert(!mixer.Apply(command).triggered);
    command.value = 1.f; assert(mixer.Apply(command).triggered);
    command.trigger = TriggerEdge::Falling; command.value = 0.f;
    assert(mixer.Apply(command).triggered);
    command.trigger = TriggerEdge::Momentary; command.value = 1.f;
    assert(mixer.Apply(command).triggered);
    command.trigger = TriggerEdge::EveryEvent;
    assert(mixer.Apply(command).triggered); assert(mixer.Apply(command).triggered);
    ParameterMixer<1, 1> tiny;
    descriptor.id = 1; descriptor.type = ParameterType::Continuous; descriptor.initial = 0;
    descriptor.operations = OperationBit(RouteOp::AddOffset);
    assert(tiny.Register(descriptor));
    command.target = 1; command.operation = RouteOp::AddOffset; command.source = {1, 1};
    assert(tiny.Apply(command)); command.source = {2, 1}; assert(!tiny.Apply(command));
    tiny.ResetSource({1, 1}); assert(tiny.Apply(command));

    Routes<4> routes;
    Route route;
    route.id = 10; route.target = 0x1234; route.number = 74;
    route.listen = Listen::Channel(16, 7); route.minimum = -0.5f; route.maximum = 0.7f;
    route.operation = RouteOp::AddOffset; route.reduction = Reduction::Sum;
    route.bipolar = true; route.center = 64;
    assert(routes.Add(route));
    route.id = 11; route.kind = ControllerKind::Nrpn; route.number = 128;
    route.minimum = 0; route.maximum = 1; route.bipolar = false; route.center = 0xffff;
    route.operation = RouteOp::EditBase;
    assert(routes.Add(route));
    std::array<uint8_t, RouteCodec<4>::kSize> bytes{};
    assert(RouteCodec<4>::Encode(routes, bytes.data(), bytes.size()));
    Routes<4> restored;
    assert(RouteCodec<4>::Decode(bytes.data(), bytes.size(), restored));
    assert(restored.Size() == 2 && restored.At(0)->target == 0x1234);
    assert(restored.At(0)->listen.channel_mask == 0x8000 && restored.At(0)->listen.port == 7);
    Near(restored.At(0)->minimum, -0.5f); Near(restored.At(0)->maximum, 0.7f);
    assert(restored.At(1)->kind == ControllerKind::Nrpn);
    for (size_t i = 0; i < bytes.size(); ++i) {
        auto corrupted = bytes;
        corrupted[i] ^= 1;
        assert(!RouteCodec<4>::Decode(corrupted.data(), corrupted.size(), restored));
        assert(restored.Size() == 2 && restored.At(0)->id == 10);
    }
    auto malformed = bytes;
    malformed[4] = 99; FixCrc(malformed.data(), malformed.size());
    assert(!RouteCodec<4>::Decode(malformed.data(), malformed.size(), restored));
    malformed = bytes;
    // A valid checksum must not make a NaN range admissible.
    malformed[16 + 24] = 0; malformed[16 + 25] = 0;
    malformed[16 + 26] = 0xc0; malformed[16 + 27] = 0x7f;
    FixCrc(malformed.data(), malformed.size());
    assert(!RouteCodec<4>::Decode(malformed.data(), malformed.size(), restored));
    assert(restored.At(0)->id == 10);
    assert(!RouteCodec<4>::Decode(bytes.data(), bytes.size() - 1, restored));
    RoutesSerializable<4> serializable(routes);
    assert(serializable.SerializedSize() == bytes.size());
    serializable.Serialize(bytes.data());
    routes.Clear(); assert(serializable.Deserialize(bytes.data())); assert(routes.Size() == 2);
    assert(RouteCodec<4>::SchemaHash() != RouteCodec<3>::SchemaHash());
    malformed = bytes;
    malformed[16 + 20] = 1; FixCrc(malformed.data(), malformed.size());
    assert(!RouteCodec<4>::Decode(malformed.data(), malformed.size(), restored));
    Route scoped = *routes.At(0); scoped.id = 99; scoped.session = 123;
    assert(routes.Add(scoped));
    assert(!RouteCodec<4>::CanEncode(routes));
    assert(!RouteCodec<4>::Encode(routes, bytes.data(), bytes.size()));
    assert(routes.Remove(99)); scoped.number = 22; assert(routes.Add(scoped));
    assert(RouteCodec<4>::Encode(routes, bytes.data(), bytes.size()));
    assert(RouteCodec<4>::Decode(bytes.data(), bytes.size(), restored));
    assert(restored.At(2)->session == 0);

    assert(mixer.SetBase(1, 0.5f));
    std::array<ParameterBase, 2> bases{{{1, 0.2f}, {2, 3.f}}};
    assert(mixer.RestoreBases(bases.data(), bases.size())); Near(mixer.Base(1), 0.2f);
    bases[0].value = 0.7f; bases[1].value = 3.5f;
    assert(!mixer.RestoreBases(bases.data(), bases.size())); Near(mixer.Base(1), 0.2f);
    bases[1] = {1, 0.3f}; assert(!mixer.RestoreBases(bases.data(), bases.size()));
    size_t persistent = 0;
    mixer.VisitPersistentBases([&](ParameterBase) { ++persistent; }); assert(persistent == 2);
    command.target = 1; command.source = {kAnyPort, 1}; command.operation = RouteOp::EditBase;
    assert(!mixer.Apply(command));
    command.source = {1, 0}; assert(!mixer.Apply(command));
    command.target = 2; command.source = {1, 1}; command.operation = RouteOp::Select;
    command.value = std::numeric_limits<float>::max(); assert(mixer.Apply(command)); Near(mixer.Value(2), 4.f);
    command.target = 1; command.operation = RouteOp::Multiply; command.route = 100;
    command.reduction = Reduction::Product; command.value = 0.5f;
    mixer.ClearModulation(); assert(mixer.Apply(command));
    command.source = {2, 1}; assert(mixer.Apply(command)); Near(mixer.Value(1), 0.05f);
    mixer.ResetSource({1, 1}); Near(mixer.Value(1), 0.1f);
    command.value = 0; assert(mixer.Apply(command)); Near(mixer.Value(1), 0.f);
    mixer.ClearModulation();

    TakeoverTracker<2> takeover;
    Route control; control.id = 1; control.target = 1; control.number = 20;
    control.takeover = TakeoverPolicy::Pickup; control.pickup_tolerance = 0.001f;
    command = {};
    command.target = 1; command.route = 1; command.source = {1, 1};
    command.operation = RouteOp::EditBase; command.value = 0.1f;
    auto take = takeover.Resolve(command, control, 0.5f, 1);
    assert(take.ok && !take.accepted);
    command.value = 0.6f; take = takeover.Resolve(command, control, 0.5f, 1);
    assert(take.accepted); assert(takeover.Acknowledge(command, 2));
    command.value = 0.7f; take = takeover.Resolve(command, control, 0.6f, 2);
    assert(take.accepted);
    command.value = 0.8f; take = takeover.Resolve(command, control, 0.2f, 3);
    assert(!take.accepted);
    command.value = 0.1f; take = takeover.Resolve(command, control, 0.2f, 3);
    assert(take.accepted);
    takeover.Clear(); control.takeover = TakeoverPolicy::Scale; command.value = 0.5f;
    take = takeover.Resolve(command, control, 0.8f, 1); assert(take.accepted); Near(take.command.value, 0.8f);
    assert(takeover.Acknowledge(command, 2)); command.value = 1.f;
    take = takeover.Resolve(command, control, 0.8f, 2); Near(take.command.value, 1.f);
    assert(takeover.Acknowledge(command, 3)); command.value = 0.f;
    take = takeover.Resolve(command, control, 1.f, 3); Near(take.command.value, 0.f);
    takeover.ResetSource(command.source); command.value = 0.7f;
    take = takeover.Resolve(command, control, 0.2f, 4); Near(take.command.value, 0.2f);
    takeover.Clear(); control.minimum = 1; control.maximum = 0; command.value = 0.5f;
    take = takeover.Resolve(command, control, 0.2f, 1); Near(take.command.value, 0.2f);
    assert(takeover.Acknowledge(command, 2)); command.value = 0;
    take = takeover.Resolve(command, control, 0.2f, 2); Near(take.command.value, 0.f);
    takeover.Clear(); control.minimum = 0; control.maximum = 0.5f;
    command.value = 0.2f; take = takeover.Resolve(command, control, 0.8f, 1);
    assert(take.ok && !take.accepted);
    auto malformed_command = command;
    malformed_command.operation = RouteOp::RelativeBase;
    assert(!takeover.Resolve(malformed_command, control, 0.2f, 1).ok);
    malformed_command = command; malformed_command.reduction = Reduction::Product;
    assert(!takeover.Resolve(malformed_command, control, 0.2f, 1).ok);
    malformed_command = command; malformed_command.channel = 1;
    assert(!takeover.Resolve(malformed_command, control, 0.2f, 1).ok);
    control.listen.port = 1; malformed_command = command; malformed_command.source.port = 2;
    assert(!takeover.Resolve(malformed_command, control, 0.2f, 1).ok);
    control.session = 1; malformed_command = command; malformed_command.source.session = 2;
    assert(!takeover.Resolve(malformed_command, control, 0.2f, 1).ok);
    control.kind = ControllerKind::Rpn; control.takeover = TakeoverPolicy::Immediate;
    command.operation = RouteOp::RelativeBase; command.value = -control.step;
    assert(takeover.Resolve(command, control, 0.2f, 1).accepted);
    takeover.Clear(); control.kind = ControllerKind::Cc7; control.takeover = TakeoverPolicy::Scale;
    control.minimum = 1e8f; control.maximum = 1e-10f;
    command.operation = RouteOp::EditBase; command.value = control.minimum;
    assert(takeover.Resolve(command, control, control.minimum, 1).accepted);
    command.value = control.maximum;
    take = takeover.Resolve(command, control, control.minimum, 1);
    assert(take.accepted && take.command.value == control.maximum);
    ParameterDescriptor oversized;
    oversized.id = 200; oversized.type = ParameterType::Selector;
    oversized.maximum = 16777216.f; oversized.operations = OperationBit(RouteOp::Select);
    assert(!oversized.Valid());
    oversized.maximum = 16777215.f;
    ParameterMixer<1, 1> large_selector;
    assert(large_selector.Register(oversized));
    for (float index : {8388609.f, 16777215.f}) {
        assert(large_selector.SetBase(oversized.id, index));
        assert(large_selector.Base(oversized.id) == index);
        assert(large_selector.Value(oversized.id) == index);
    }
    Routes<2> validated;
    Route valid; valid.id = 10; valid.target = 1; valid.number = 20;
    assert(validated.Add(valid)); assert(validated.ValidateTargets(mixer));
    valid.id = 11; valid.target = 999; valid.number = 21;
    assert(validated.Add(valid)); assert(!validated.ValidateTargets(mixer));
    validated.Clear(); valid.target = 2; valid.minimum = 0; valid.maximum = 5;
    valid.operation = RouteOp::Select; assert(validated.Add(valid)); assert(!validated.ValidateTargets(mixer));
    Routes<4> collision;
    Route duplicate = *routes.At(0); assert(collision.Add(duplicate));
    duplicate.id = 77; duplicate.session = 123; assert(collision.Add(duplicate));
    RoutesSerializable<4> fail_closed(collision);
    assert(!fail_closed.CanSerialize()); bytes.fill(0xab); fail_closed.Serialize(bytes.data());
    for (auto byte : bytes) assert(byte == 0);

    ParameterMixer<1,2> unclamped; ParameterDescriptor unit;unit.id=500;
    assert(unclamped.Register(unit));assert(unclamped.SetBase(500,.8f));
    assert(std::fabs(unclamped.Compose(500,.5f,-.4f)-.9)<1e-5);
    assert(unclamped.Compose(500,.5f)>1.f && unclamped.Value(500,.5f)==1.f);
    std::puts("MIDI parameter lanes, ownership, persistence passed");
}
