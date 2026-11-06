#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>
#include "alchemy/midi/controller.h"
#include "alchemy/midi/learn.h"

using namespace alchemy::midi;

static void ProgramIndices() {
    Routes<1> routes;
    Route route;
    route.id = route.target = 1;
    route.kind = ControllerKind::Program;
    route.operation = RouteOp::Select;
    route.minimum = 0; route.maximum = 7;
    route.number = 129;
    assert(routes.Add(route));
    ControllerMessage message;
    message.kind = ControllerKind::Program; message.number = 129;
    unsigned count = 0;
    for (int i = 0; i < 8; ++i) {
        message.value = i;
        routes.Process(message, [&](const ParameterCommand& command) { assert(command.value == i); ++count; });
    }
    message.value = 8; routes.Process(message, [&](const ParameterCommand&) { assert(false); });
    message.value = 0; message.number = 128;
    routes.Process(message, [&](const ParameterCommand&) { assert(false); });
    assert(count == 8);
    routes.Clear(); route.minimum = 7; route.maximum = 0;
    assert(routes.Add(route)); message.number = 129; message.value = 3;
    routes.Process(message, [&](const ParameterCommand& command) { assert(command.value == 3); ++count; });
    assert(count == 9);
}

static void WideRangeEndpoints() {
    for (float sign : {-1.f, 1.f}) {
        Route route;
        route.id = route.target = 1; route.number = 20;
        route.minimum = sign * 1e8f; route.maximum = sign * 1e-10f;
        Routes<1> routes;
        assert(routes.Add(route));
        ControllerMessage message;
        message.number = route.number;
        for (int endpoint : {0, 127}) {
            message.value = endpoint;
            unsigned count = 0;
            routes.Process(message, [&](const ParameterCommand& command) {
                assert(command.value == (endpoint ? route.maximum : route.minimum));
                ++count;
            });
            assert(count == 1);
        }
    }
}

int main() {
    ProgramIndices();
    WideRangeEndpoints();
    ControllerDecoder<4> decoder;
    std::vector<ControllerMessage> messages;
    auto sink = [&](const ControllerMessage& m) { messages.push_back(m); };
    auto cc = [&](SourceId s, uint8_t ch, uint8_t number, uint8_t value) {
        messages.clear();
        assert(decoder.Process(Event::Cc(s, ch, number, value), sink));
    };
    SourceId a{1, 1}, b{2, 1};
    cc(a, 0, 16, 10);
    assert(messages.size() == 2 && messages.back().kind == ControllerKind::Cc14 && messages.back().value == 1280);
    cc(a, 0, 48, 127);
    assert(messages.back().value == 1407);
    cc(a, 0, 16, 11);
    assert(messages.back().value == 1408);
    cc(b, 0, 48, 127);
    assert(messages.back().value == 127);
    cc(a, 1, 48, 64);
    assert(messages.back().value == 64);

    cc(a, 0, 101, 0); cc(a, 0, 100, 0); cc(a, 0, 6, 2);
    assert(messages.back().kind == ControllerKind::Rpn && messages.back().number == 0 && messages.back().value == 256);
    cc(a, 0, 38, 50);
    assert(messages.back().value == 306);
    cc(a, 0, 96, 127);
    assert(messages.back().kind == ControllerKind::RpnDelta && messages.back().value == 1);
    cc(a, 0, 97, 5);
    assert(messages.back().value == -1);
    cc(a, 0, 100, 1); cc(a, 0, 38, 4);
    assert(messages.back().number == 1 && messages.back().value == 4);
    cc(a, 0, 101, 127); cc(a, 0, 100, 127); cc(a, 0, 6, 8);
    assert(messages.size() == 2);
    cc(b, 0, 99, 1); cc(b, 0, 98, 3); cc(b, 0, 6, 5);
    assert(messages.back().kind == ControllerKind::Nrpn && messages.back().number == 131 && messages.back().value == 640);
    cc(a, 0, 6, 9);
    assert(messages.size() == 2);
    cc(b, 0, 0, 10); cc(b, 0, 32, 11); cc(b, 0, 121, 0);
    assert(messages.back().kind == ControllerKind::Reset && messages.back().channel == 0
        && messages.back().Valid());
    Event program = Event::System(EventType::ProgramChange, b, 0, 12);
    assert(decoder.Process(program, sink));
    assert(messages.back().kind == ControllerKind::Program && messages.back().number == 1291 && messages.back().value == 12);
    assert(decoder.Process(Event::Bend(a, 0, 8192), sink));
    assert(messages.back().kind == ControllerKind::Bend && messages.back().value == 8192);
    cc(a, 0, 10, 64); cc(a, 0, 42, 3); cc(a, 0, 121, 0); cc(a, 0, 42, 4);
    assert(messages.back().value == 8196);
    assert(decoder.Process(Event::System(EventType::SourceReset, b), sink));
    assert(messages.back().kind == ControllerKind::Disconnect && messages.back().channel == kAnyChannel
        && messages.back().Valid());
    auto malformed_reset = messages.back(); malformed_reset.channel = 0;
    assert(!malformed_reset.Valid());
    assert(decoder.Process(Event::System(EventType::SystemReset, b), sink));
    assert(messages.back().kind == ControllerKind::Reset && messages.back().channel == kAnyChannel
        && messages.back().Valid());
    malformed_reset = messages.back(); malformed_reset.channel = 0;
    assert(!malformed_reset.Valid());
    malformed_reset.number = 121; assert(malformed_reset.Valid());
    malformed_reset.channel = kAnyChannel; assert(!malformed_reset.Valid());
    program.value = 1;
    assert(decoder.Process(program, sink));
    assert(messages.back().number == 0);
    Event invalid = Event::Cc(a, 16, 20, 1);
    assert(!decoder.Process(invalid, sink));
    invalid = Event::Bend(a, 0, 16384);
    assert(!decoder.Process(invalid, sink));
    invalid = Event::Cc({kAnyPort, 1}, 0, 20, 1);
    assert(!decoder.Process(invalid, sink));
    ControllerDecoder<1> tiny;
    assert(tiny.Process(Event::Cc(a, 0, 20, 1), sink));
    assert(!tiny.Process(Event::Cc(b, 0, 20, 1), sink));
    tiny.ResetSource(a);
    assert(tiny.Process(Event::Cc(b, 0, 20, 1), sink));

    assert(DecodeRelative(127, RelativeEncoding::TwosComplement) == -1);
    assert(DecodeRelative(1, RelativeEncoding::TwosComplement) == 1);
    assert(DecodeRelative(64, RelativeEncoding::BinaryOffset) == 0);
    assert(DecodeRelative(65, RelativeEncoding::SignedBit) == -1);
    assert(DecodeRelative(65, RelativeEncoding::SignedBit2) == 1);

    Routes<4> routes;
    Route route;
    route.id = 1; route.target = 10; route.number = 20;
    route.listen = Listen::Channel(1, 1);
    route.bipolar = true; route.minimum = -1.f; route.maximum = 1.f;
    route.operation = RouteOp::AddOffset;
    assert(routes.Add(route));
    assert(!routes.Add(route));
    float mapped = 999.f;
    auto mapped_sink = [&](const ParameterCommand& command) { mapped = command.value; };
    ControllerMessage message;
    message.source = a; message.number = 20; message.value = 64;
    routes.Process(message, mapped_sink);
    assert(mapped == 0.f);
    message.value = 0; routes.Process(message, mapped_sink); assert(mapped == -1.f);
    message.value = 127; routes.Process(message, mapped_sink); assert(mapped == 1.f);
    message.source = b; mapped = 999.f; routes.Process(message, mapped_sink); assert(mapped == 999.f);
    route.id = 2; route.number = 64; assert(!routes.Add(route));
    route.advanced = true; assert(routes.Add(route));
    assert(routes.Remove(2)); assert(!routes.Remove(2));

    LearnSession learn;
    Route prototype; prototype.id = 8; prototype.target = 11;
    LearnOptions options; options.listen = Listen::Omni();
    assert(learn.Arm(prototype, options, 0));
    message.source = b; message.channel = 4; message.number = 64;
    assert(!learn.Process(message, 1));
    message.number = 23;
    assert(learn.Process(message, 2));
    assert(learn.Status() == LearnStatus::Captured);
    assert(learn.Result()->number == 23 && learn.Result()->listen.port == 2
        && learn.Result()->listen.channel_mask == (1u << 4));
    assert(learn.Accept()); assert(!learn.Accept());
    options.timeout_us = 32;
    assert(learn.Arm(prototype, options, 0xfffffff0u));
    learn.Poll(5); assert(learn.Status() == LearnStatus::Armed);
    learn.Poll(0x10); assert(learn.Status() == LearnStatus::TimedOut);
    learn.Cancel(); assert(learn.Status() == LearnStatus::Idle);
    options.kind = ControllerKind::Cc14;
    assert(learn.Arm(prototype, options, 0));
    assert(!learn.Process(message, 1));
    message.kind = ControllerKind::Cc14; message.number = 16; message.resolution = 14;
    assert(learn.Process(message, 1));
    message.kind = ControllerKind::Cc7; message.resolution = 7; message.source = a;
    message.channel = 0; message.number = 20; message.value = 64;
    mapped = 999.f;
    message.resolution = 14; routes.Process(message, mapped_sink); assert(mapped == 999.f);
    message.resolution = 7; message.source.session = 0;
    routes.Process(message, mapped_sink); assert(mapped == 999.f);
    message.source = a; message.value = -1;
    routes.Process(message, mapped_sink); assert(mapped == 999.f);
    Route bad = route;
    bad.id = 100; bad.number = 20; bad.center = 0;
    assert(!bad.Valid());
    bad = route; bad.center = 128; assert(!bad.Valid());
    bad = route; bad.operation = RouteOp::Multiply; assert(!bad.Valid());
    bad = route; bad.kind = ControllerKind::Cc14; bad.number = 6; assert(bad.Valid());
    bad = route; bad.kind = ControllerKind::Rpn; bad.operation = RouteOp::RelativeBase;
    assert(!bad.Valid());
    Route shaped; shaped.id = 2; shaped.target = 3; shaped.number = 20;
    shaped.curve = Curve::Power; shaped.exponent = 2.f;
    Routes<1> curved; assert(curved.Add(shaped));
    message.value = 64; curved.Process(message, mapped_sink);
    assert(std::fabs(mapped - (64.f / 127.f) * (64.f / 127.f)) < 1e-6f);
    shaped.bipolar = true; shaped.minimum = -1; shaped.maximum = 1;
    curved.Clear(); assert(curved.Add(shaped));
    curved.Process(message, mapped_sink); assert(mapped == 0.f);
    message.value = 0; curved.Process(message, mapped_sink); assert(mapped == -1.f);
    message.value = 127; curved.Process(message, mapped_sink); assert(mapped == 1.f);
    cc(a, 0, 6, 3);
    bool found = false;
    for (const auto& m : messages) if (m.kind == ControllerKind::Cc14 && m.number == 6) {
        assert(m.value == 384); found = true;
    }
    assert(found);
    cc(a, 0, 38, 7);
    found = false;
    for (const auto& m : messages) if (m.kind == ControllerKind::Cc14 && m.number == 6) {
        assert(m.value == 391); found = true;
    }
    assert(found);
    shaped.bipolar = false; shaped.center = 0xffff; shaped.curve = Curve::Linear;
    shaped.minimum = -std::numeric_limits<float>::max(); shaped.maximum = std::numeric_limits<float>::max();
    curved.Clear(); assert(curved.Add(shaped));
    message.kind = ControllerKind::Cc7; message.resolution = 7; message.number = 20; message.value = 64;
    curved.Process(message, mapped_sink); assert(std::isfinite(mapped) && mapped > 0.f);
    std::puts("MIDI controllers, routes, learn passed");
}
