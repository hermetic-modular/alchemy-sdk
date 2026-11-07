#include "alchemy/midi/ingress.h"
#include <cassert>
#include <cstdio>
#include <thread>

using namespace alchemy::midi;

int main() {
    const SourceId a{0, 1}, b{1, 1};
    assert(Event::NoteOn(a, 0, 60, 0).type == EventType::NoteOff);
    auto shorthand = Event::NoteOn(a, 0, 60, 0);
    assert(shorthand.Valid());
    shorthand.value = 1;
    assert(!shorthand.Valid());
    assert(!Event::NoteOn(a, 16, 60, 1).Valid());
    assert(!Event::Bend(a, 0, 16384).Valid());
    assert(Listen::Channel(16).Matches(Event::NoteOn(a, 15, 60, 1)));
    assert(!Listen::Channel(0).Matches(Event::NoteOn(a, 0, 60, 1)));
    assert(Bipolar7(0) == -1 && Bipolar7(64) == 0 && Bipolar7(127) == 1);
    assert(Bipolar14(0) == -1 && Bipolar14(8192) == 0 && Bipolar14(16383) == 1);
    assert(TimeReached(2, 0xfffffffeu));
    Queue<uint32_t, 128> queue;
    constexpr uint32_t count = 1000000;
    std::thread producer([&] {
        for (uint32_t i = 0; i < count; ++i)
            while (!queue.Push(i)) std::this_thread::yield();
    });
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t value;
        while (!queue.Pop(value)) std::this_thread::yield();
        assert(value == i);
    }
    producer.join();
    assert(queue.Size() == 0);

    Ingress<4, 2> in;
    assert(in.Push(Event::NoteOn(a, 0, 60, 100)));
    assert(in.Push(Event::NoteOn(b, 0, 61, 100)));
    assert(in.Push(Event::Cc(a, 0, 1, 64)));
    assert(in.Push(Event::Cc(b, 0, 1, 64)));
    assert(!in.Push(Event::NoteOff(a, 0, 60)));
    assert(in.Failed(a) && !in.Failed(b));
    int resets = 0, b_events = 0;
    in.Drain([&](const Event& e) {
        if (e.type == EventType::SourceReset) { assert(e.source == a); ++resets; }
        else { assert(e.source == b); ++b_events; }
    });
    assert(resets == 1 && b_events == 2 && in.Lost() == 1);
    assert(!in.Push(Event::NoteOn(a, 0, 60, 100)));
    assert(in.Push(Event::NoteOn({0, 2}, 0, 60, 100, 100)));
    int delivered = 0;
    assert(in.DrainDue(99, [&](const Event&) { ++delivered; }) == 0);
    assert(in.DrainDue(100, [&](const Event&) { ++delivered; }) == 1);
    assert(delivered == 1);

    Ingress<4, 2> consecutive;
    for (int i = 0; i < 4; ++i) assert(consecutive.Push(Event::NoteOn(a, 0, 60, 1)));
    assert(!consecutive.Push(Event::NoteOff(a, 0, 60)));
    int faults = 0;
    consecutive.Drain([&](const Event& e) { assert(e.type == EventType::SourceReset); ++faults; }, 1);
    assert(consecutive.Push(Event::NoteOn({0, 2}, 0, 61, 1)));
    assert(!consecutive.Push(Event::NoteOff({0, 2}, 0, 61)));
    consecutive.Drain([&](const Event& e) { assert(e.type == EventType::SourceReset); ++faults; });
    assert(faults == 2);
    assert(!consecutive.Push(Event::NoteOn(a, 0, 62, 1)));
    assert(consecutive.Push(Event::NoteOn({0, 3}, 0, 62, 1)));
    std::puts("MIDI core tests passed (1,000,000 concurrent transfers)");
}
