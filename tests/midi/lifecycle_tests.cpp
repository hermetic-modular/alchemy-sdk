#include "alchemy/midi/control_edits.h"
#include "alchemy/midi/dispatcher.h"
#include "alchemy/midi/ingress.h"
#include "alchemy/midi/receiver.h"
#include "alchemy/midi/safety.h"
#include "alchemy/midi/snapshot.h"
#include "alchemy/midi/thru.h"
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

using namespace alchemy::midi;

struct Configuration { uint32_t a = 0, b = 0, c = 0, d = 0; };

static void Snapshots() {
    SnapshotExchange<Configuration> exchange;
    const auto valid = [](const Configuration& c) { return c.a && c.b == c.a * 2 && c.c == c.a * 3 && c.d == c.a * 4; };
    assert(!exchange.Publish({}, valid));
    assert(exchange.Publish({1, 2, 3, 4}, valid));
    SnapshotExchange<Configuration>::Lease first;
    assert(exchange.Acquire(first));
    const auto revision = first.Revision();
    assert(!exchange.Acquire(first));
    assert(exchange.Publish({2, 4, 6, 8}, valid));
    assert(exchange.Publish({3, 6, 9, 12}, valid));
    assert(first->a == 1 && first.Revision() == revision);
    first.Release();
    assert(exchange.Acquire(first) && first->a == 3);
    first.Release();
    constexpr uint32_t iterations = 100000;
    std::thread writer([&] {
        for (uint32_t i = 4; i <= iterations; ++i)
            while (!exchange.Publish({i, i * 2, i * 3, i * 4}, valid)) std::this_thread::yield();
    });
    uint32_t last = 3;
    while (last != iterations) {
        if (!exchange.Acquire(first)) { std::this_thread::yield(); continue; }
        assert(valid(*first) && first->a > last);
        last = first->a;
        first.Release();
    }
    writer.join();
}

static void Dispatching() {
    Dispatcher<2, 4, 1> dispatcher;
    std::vector<Event> events;
    const Observer observer{&events, [](void* context, const Event& e) {
        static_cast<std::vector<Event>*>(context)->push_back(e);
    }};
    assert(dispatcher.Subscribe(observer) && !dispatcher.Subscribe(observer));
    assert(dispatcher.Attach({0, 1}) && dispatcher.Attach({1, 1}));
    assert(dispatcher.Push(Event::NoteOn({0, 1}, 0, 60, 1, 10)));
    assert(dispatcher.Push(Event::NoteOn({0, 1}, 0, 62, 1, 30)));
    assert(dispatcher.Push(Event::NoteOn({1, 1}, 0, 61, 1, 20)));
    assert(dispatcher.Poll(25) == 2 && events[0].data1 == 60 && events[1].data1 == 61);
    assert(dispatcher.Poll(30) == 1 && events.back().data1 == 62);
    events.clear();
    assert(dispatcher.Push(Event::NoteOn({1, 1}, 0, 66, 1, 30)));
    assert(dispatcher.Push(Event::NoteOn({0, 1}, 0, 65, 1, 30)));
    assert(dispatcher.Push(Event::NoteOn({1, 1}, 0, 67, 1, 30)));
    assert(dispatcher.Poll(30) == 3 && events.size() == 3);
    assert(events[0].data1 == 66 && events[1].data1 == 65 && events[2].data1 == 67);
    events.clear();
    for (int i = 0; i < 4; ++i) assert(dispatcher.Push(Event::NoteOn({0, 1}, 0, 60, 1, 31 + i)));
    assert(!dispatcher.Push(Event::NoteOff({0, 1}, 0, 60, 0, 36)));
    assert(dispatcher.Push(Event::NoteOn({1, 1}, 0, 61, 1, 35)));
    assert(dispatcher.Poll(40) == 1 && events.size() == 2);
    assert(events[0].type == EventType::SourceReset && events[1].source.port == 1);
    assert(!dispatcher.Attach({0, 1}) && dispatcher.Attach({0, 2}));
    events.clear();
    assert(dispatcher.Push(Event::NoteOn({0, 2}, 0, 64, 1, 41)));
    dispatcher.Poll(41);
    assert(events[0].type == EventType::SourceReset && events[0].source.session == 1);
    assert(events[1].source.session == 2 && events[1].sequence > events[0].sequence);
    assert(!dispatcher.Push(Event::NoteOn({0, 1}, 0, 65, 1, 42)));
    assert(!dispatcher.Push(Event::NoteOn({0, 2}, 0, 65, 1, 40)));
    dispatcher.Poll(42);
    assert(events.back().type == EventType::SourceReset);

    Dispatcher<1, 4, 1> wrap;
    assert(wrap.Subscribe(observer) && wrap.Attach({0, 0xffffffffu}));
    assert(wrap.Push(Event::NoteOn({0, 0xffffffffu}, 0, 60, 1, 0xfffffff0u)));
    assert(wrap.Push(Event::NoteOff({0, 0xffffffffu}, 0, 60, 0, 10)));
    assert(wrap.Poll(0) == 1 && wrap.Poll(10) == 1);
    assert(wrap.Attach({0, 1}));

    Dispatcher<1, 2, 1> handoff;
    assert(handoff.Subscribe(observer) && handoff.Attach({0, 1}));
    assert(handoff.Push(Event::NoteOn({0, 1}, 0, 60, 1, 10)));
    handoff.Poll(10);
    assert(handoff.Attach({0, 2}));
    assert(!handoff.Attach({0, 3}));
    assert(handoff.Push(Event::NoteOn({0, 2}, 0, 62, 1, 11)));
    assert(handoff.Push(Event::NoteOn({0, 2}, 0, 64, 1, 12)));
    assert(!handoff.Push(Event::NoteOff({0, 2}, 0, 62, 0, 13)));
    events.clear();
    assert(handoff.Poll(20) == 0 && events.size() == 2);
    assert(events[0].type == EventType::SourceReset && events[0].source.session == 1);
    assert(events[1].type == EventType::SourceReset && events[1].source.session == 2);
    assert(handoff.Attach({0, 3}));
    assert(handoff.Push(Event::System(EventType::SourceReset, {0, 3}, 21)));
    assert(handoff.Push(Event::System(EventType::SourceReset, {0, 3}, 22)));
    events.clear();
    assert(handoff.Poll(30) == 0 && events.size() == 2);
    assert(events[0].source.session == 2 && events[1].source.session == 3);
    assert(handoff.Push(Event::NoteOn({0, 3}, 0, 65, 1, 31)));
    assert(handoff.Poll(31) == 1 && events.back().type == EventType::NoteOn);
}

static void IdleSource() {
    Dispatcher<1, 4, 1> dispatcher;
    std::vector<Event> delivered;
    assert(dispatcher.Subscribe(Observer{&delivered, [](void* p, const Event& e) {
        static_cast<std::vector<Event>*>(p)->push_back(e);
    }}));
    const SourceId source{0, 1};
    const uint32_t start = 0xffff0000u;
    assert(dispatcher.Accept(Event::NoteOff(source, 0, 60, 0, start)));
    assert(dispatcher.Poll(start) == 1);
    constexpr uint32_t silence = 36u * 60u * 1000000u;
    for (uint32_t elapsed = 1000000; elapsed < silence; elapsed += 1000000)
        dispatcher.Poll(start + elapsed);
    assert(dispatcher.Accept(Event::NoteOn(source, 0, 62, 100, start + silence)));
    assert(dispatcher.Poll(start + silence) == 1);
    assert(delivered.size() == 2 && delivered.back().type == EventType::NoteOn);
    assert(!dispatcher.Failed(source) && dispatcher.Lost() == 0);
}

static void RecoveryComposition() {
    const auto sample = [](uint32_t now) { return alchemy::TickTimebase::Sample{now, now, 1000000, 0}; };
    const uint8_t chord[] = {0x90, 60, 100, 0x90, 61, 100, 0x90, 62, 100};
    const uint8_t fresh[] = {0x90, 65, 100};
    const auto reset = Event::System(EventType::SourceReset, {0, 1}, 100);
    Ingress<2, 1> direct;
    Receiver<16> receiver;
    assert(receiver.Init({0, 1}, 1000000));
    receiver.Receive(chord, sizeof(chord), 100);
    auto receive = [&](const Event& e) { return direct.Push(e); };
    receiver.Poll(sample(100), receive);
    assert(receiver.Faulted() && !direct.Push(reset));
    unsigned resets = 0, notes = 0;
    const auto audio = [&](const Event& e) {
        if (e.type == EventType::SourceReset) { ++resets; assert(e.timestamp_valid && e.time_us == 100); }
        else ++notes;
    };
    direct.Drain(100, audio);
    receiver.Poll(sample(100), receive);
    assert(resets == 1 && notes == 0 && receiver.ResetReported());
    assert(direct.Push(reset) && !direct.Push(Event::NoteOn({0, 1}, 0, 64, 100)));
    assert(receiver.Restart({0, 2}));
    receiver.Receive(fresh, sizeof(fresh), 101);
    receiver.Poll(sample(101), receive);
    direct.Drain(101, [&](const Event& e) { assert(e.type == EventType::NoteOn && e.source.session == 2); ++notes; });
    assert(notes == 1);

    Dispatcher<1, 8, 1> dispatcher;
    Ingress<2, 1> ingress;
    assert(dispatcher.Subscribe(CriticalSink{&ingress, [](void* p, const Event& e) {
        return static_cast<Ingress<2, 1>*>(p)->Push(e);
    }}));
    Receiver<16> upstream;
    assert(upstream.Init({0, 1}, 1000000));
    auto forward = [&](const Event& e) { return dispatcher.Accept(e); };
    upstream.Receive(chord, sizeof(chord), 100);
    upstream.Poll(sample(100), forward);
    assert(dispatcher.Poll(100) == 3 && dispatcher.Failed(upstream.Source()) && dispatcher.Lost() == 1);
    upstream.MarkLost();
    upstream.Poll(sample(100), forward);
    dispatcher.Poll(100);
    assert(!upstream.ResetReported());
    resets = notes = 0;
    ingress.Drain(100, audio);
    dispatcher.Poll(100);
    upstream.Poll(sample(100), forward);
    assert(upstream.ResetReported() && resets == 1 && notes == 0);
    assert(upstream.Restart({0, 2}));
    upstream.Receive(fresh, sizeof(fresh), 101);
    upstream.Poll(sample(101), forward);
    dispatcher.Poll(101);
    ingress.Drain(101, [&](const Event& e) { assert(e.type == EventType::NoteOn && e.source.session == 2); ++notes; });
    assert(notes == 1 && !dispatcher.Failed(upstream.Source()));
    assert(!dispatcher.Accept(Event::NoteOn({0, 1}, 0, 66, 100, 102)));

    Dispatcher<1, 2, 1> bounded;
    std::vector<Event> delivered;
    assert(bounded.Subscribe(Observer{&delivered, [](void* p, const Event& e) {
        static_cast<std::vector<Event>*>(p)->push_back(e);
    }}));
    Receiver<16> third;
    assert(third.Init({0, 1}, 1000000));
    third.Receive(chord, sizeof(chord), 100);
    auto dispatch = [&](const Event& e) { return bounded.Accept(e); };
    third.Poll(sample(100), dispatch);
    third.Poll(sample(100), dispatch);
    assert(third.Faulted() && !third.ResetReported());
    bounded.Poll(100);
    third.Poll(sample(100), dispatch);
    assert(third.ResetReported() && delivered.size() == 1 && delivered[0].type == EventType::SourceReset);

    Ingress<2, 1> untimed;
    auto recovery = Event::System(EventType::SourceReset, {0, 1});
    recovery.timestamp_valid = false;
    assert(untimed.Push(recovery));
    assert(untimed.Push(Event::NoteOn({0, 2}, 0, 60, 100, 0x90000000u)));
    unsigned received = 0;
    assert(untimed.DrainDue(0x90000000u, [&](const Event& e) {
        assert(e.timestamp_valid && e.time_us == 0x90000000u); ++received;
    }) == 2 && received == 2);

    for (unsigned mode = 0; mode < 3; ++mode) {
        Ingress<2, 1> timed;
        assert(timed.Push(Event::NoteOn({0, 1}, 0, 60, 100)));
        assert(timed.Push(Event::NoteOn({0, 1}, 0, 61, 100)));
        assert(!timed.Push(Event::NoteOff({0, 1}, 0, 60)));
        bool seen = false;
        auto capture = [&](const Event& e) {
            assert(e.Valid() && e.type == EventType::SourceReset);
            assert(e.timestamp_valid == (mode != 0));
            assert(e.time_us == (mode == 2 ? 0x90000000u : 0));
            seen = true;
        };
        if (mode == 0) timed.Drain(capture);
        else timed.DrainDue(mode == 2 ? 0x90000000u : 0, capture);
        assert(seen);
    }
}

static void Liveness() {
    ActiveSensing<2> sensing;
    int reset = 0;
    auto sink = [&](const Event& e) { assert(e.type == EventType::SourceReset && e.source.port == 0); ++reset; };
    assert(sensing.Observe(Event::NoteOn({0, 1}, 0, 60, 100, 0)));
    assert(sensing.Poll(1000000, sink) == 0);
    assert(sensing.Observe(Event::System(EventType::ActiveSensing, {0, 1}, 0xfffffff0u)));
    assert(sensing.Poll(299982, sink) == 0);
    sensing.Touch({0, 1}, 200000); // Ongoing raw SysEx activity need not produce a decoded message.
    assert(sensing.Poll(499999, sink) == 0);
    assert(sensing.Poll(500000, sink) == 1 && reset == 1);
    assert(sensing.Poll(800000, sink) == 0);
    assert(sensing.Observe(Event::System(EventType::ActiveSensing, {0, 2}, 800000)));
    sensing.ResetSource({0, 1});
    assert(sensing.Poll(1100000, sink) == 1 && reset == 2);
}

static void EditsAndThru() {
    ControlEdits<4> edits;
    ParameterCommand command;
    command.target = 1; command.route = 1; command.value = 0.5f;
    assert(edits.PushBase(command, 0.5f, 1, 1) && edits.PushBase(command, 0.6f, 2, 1));
    int applied = 0;
    assert(edits.Drain(2, [](ParamId) { return 1u; }, [&](const ParameterCommand& c) {
        assert(c.value == 0.6f); ++applied;
    }) == 2 && applied == 1);
    command.operation = RouteOp::RelativeBase;
    assert(edits.PushBase(command, 0.7f, 2, 1));
    assert(edits.Drain(2, [](ParamId) { return 2u; }, [&](const ParameterCommand&) { assert(false); }) == 1);
    command.operation = RouteOp::AddOffset;
    assert(!edits.PushBase(command, 0.6f, 2, 1));
    Thru<2> thru;
    int sent = 0;
    auto sink = [&](uint16_t port, const Event&) { assert(port == 1); ++sent; return true; };
    assert(thru.Process(Event::NoteOn({0, 1}, 0, 60, 100), sink) == 0);
    assert(thru.Add({Listen::Omni(), 1, ClassBit(MessageClass::Notes), false}));
    assert(thru.Process(Event::NoteOn({0, 1}, 0, 60, 100), sink) == 1);
    assert(thru.Process(Event::NoteOn({1, 1}, 0, 60, 100), sink) == 0);
    assert(thru.Process(Event::System(EventType::SourceReset, {0, 1}), sink) == 0 && sent == 1);
    assert(!thru.Add({Listen::Channel(1, 0), 1, ClassBit(MessageClass::Notes)}));
    assert(thru.Add({Listen::Omni(1), 1, ClassBit(MessageClass::Notes), true}));
    assert(thru.Process(Event::NoteOn({1, 1}, 0, 60, 100), sink) == 1);
    thru.Clear();
    assert(thru.Add({Listen::Channel(1, 0), 1, ClassBit(MessageClass::Notes)}));
    assert(!thru.Add({Listen::Omni(), 1, ClassBit(MessageClass::Notes)}));
    assert(thru.Add({Listen::Channel(2, 0), 1, ClassBit(MessageClass::Notes)}));
    assert(thru.Process(Event::NoteOn({0, 1}, 0, 60, 100), sink) == 1);
    assert(thru.Process(Event::NoteOn({0, 1}, 1, 60, 100), sink) == 1);
    for (const auto type : {MessageClass::SystemCommon, MessageClass::Realtime}) {
        thru.Clear();
        assert(thru.Add({Listen::Channel(1, 0), 1, ClassBit(type)}));
        assert(!thru.Add({Listen::Channel(2, 0), 1, ClassBit(type)}));
        assert(thru.Add({Listen::Channel(2, 2), 1, ClassBit(type)}));
        const auto event = type == MessageClass::Realtime ? EventType::TimingClock : EventType::TuneRequest;
        assert(thru.Process(Event::System(event, {0, 1}), sink) == 1);
    }
}

int main() {
    Snapshots(); Dispatching(); IdleSource(); RecoveryComposition(); Liveness(); EditsAndThru();
    std::puts("MIDI lifecycle tests passed");
}
