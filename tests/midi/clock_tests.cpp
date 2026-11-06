#include "alchemy/midi/clock.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>
using namespace alchemy;
using namespace alchemy::midi;
static double Position(const MusicalClock& clock) {
    return double(clock.MasterTick()) + clock.FracTick();
}
static void Near(double actual, double expected, double tolerance = 1e-7) {
    if (!(std::fabs(actual - expected) < tolerance))
        std::fprintf(stderr,"actual %.12f expected %.12f tolerance %.12f\n",actual,expected,tolerance);
    assert(std::fabs(actual - expected) < tolerance);
}

static void ExactColdStartAndBatches() {
    const SourceId source{0,1};
    struct Case { uint32_t ppqn, period; };
    constexpr Case cases[] = {{24,125000}, {96,20833}, {192,10000}};
    for (const auto scenario : cases) {
        const auto ppqn = scenario.ppqn, period = scenario.period;
        MusicalClock regular_clock(ppqn), batch_clock(ppqn);
        ClockFollower regular_follower(regular_clock), batch_follower(batch_clock);
        ClockAdapter regular(regular_follower,regular_clock), batch(batch_follower,batch_clock);
        assert(regular.SelectSource(source) && batch.SelectSource(source));
        regular.Poll(0); batch.Poll(0);
        const auto start = Event::System(EventType::Start,source,1);
        assert(regular.Process(start) && batch.Process(start));
        uint32_t now = 0;
        for (uint32_t i = 0; i <= 96; ++i) {
            const uint32_t stamp = 1000 + i * period;
            while (now + 1000 < stamp) { now += 1000; regular.Poll(now); }
            const auto pulse = Event::System(EventType::TimingClock,source,stamp);
            assert(regular.Process(pulse) && batch.Process(pulse));
            regular.Poll(stamp); now = stamp;
            Near(Position(regular_clock), double(i) * (ppqn / 24));
            Near(Position(batch_clock), double(i) * (ppqn / 24));
            assert(regular_clock.Events() & CLK_EV::MIDI_CLOCK);
        }
        regular.Poll(now + 100); batch.Poll(now + 100);
        Near(Position(regular_clock), Position(batch_clock));
        Near(regular_clock.Bpm(), batch_clock.Bpm());
        const auto transport_events = CLK_EV::START | CLK_EV::RESET | CLK_EV::EXT_LOCK;
        assert((batch_clock.Events() & transport_events) == transport_events);
        assert(batch_clock.Events() & CLK_EV::QUARTER);
    }
}

static void TimestampedTransport() {
    const SourceId source{0,1};
    MusicalClock clock; ClockFollower follower(clock); ClockAdapter adapter(follower,clock);
    assert(adapter.SelectSource(source)); adapter.Poll(0);
    assert(adapter.Process(Event::System(EventType::Start,source,1)));
    uint32_t stamp = 0;
    for (uint32_t i = 0; i <= 24; ++i) {
        stamp = 1000 + i * 20833;
        assert(adapter.Process(Event::System(EventType::TimingClock,source,stamp)));
        adapter.Poll(stamp);
    }
    const double stopped = Position(clock) + clock.TicksPerUs() * 10000;
    assert(adapter.Process(Event::System(EventType::Stop,source,stamp + 10000)));
    Near(Position(clock),stopped); assert(!clock.Running());
    adapter.Poll(stamp + 10000); Near(Position(clock),stopped);
    assert(clock.Events() & CLK_EV::STOP);
    assert(adapter.Process(Event::System(EventType::Continue,source,stamp + 11000)));
    assert(adapter.Process(Event::System(EventType::TimingClock,source,stamp + 20833)));
    Near(Position(clock),stopped);
    assert(adapter.Process(Event::System(EventType::TimingClock,source,stamp + 41666)));
    Near(Position(clock),stopped + 4);
    const double restart_stop = Position(clock) + clock.TicksPerUs() * 10000;
    assert(adapter.Process(Event::System(EventType::Start,source,stamp + 51666)));
    Near(Position(clock),restart_stop); assert(!clock.Running());
    assert(adapter.Process(Event::System(EventType::TimingClock,source,stamp + 62499)));
    Near(Position(clock),0); assert(clock.Running());
    const double reset_position = clock.TicksPerUs() * 10000;
    assert(adapter.Process(Event::System(EventType::SourceReset,source,stamp + 72499)));
    Near(Position(clock),reset_position); assert(!clock.Running());
    assert(adapter.SelectSource({0,2}));
    assert(adapter.Process(Event::System(EventType::Start,{0,2},stamp + 72500)));
    assert(adapter.Process(Event::System(EventType::TimingClock,{0,2},stamp + 73000)));
    assert(adapter.Process(Event::System(EventType::SystemReset,{0,2},stamp + 74000)));
    Near(Position(clock),0); adapter.Poll(stamp + 74000);
    assert(!clock.Running() && (clock.Events() & CLK_EV::RESET) && (clock.Events() & CLK_EV::STOP));
}

static void LateColdBatch() {
    const SourceId source{0,1};
    MusicalClock clock; ClockFollower follower(clock); ClockAdapter adapter(follower,clock);
    assert(adapter.SelectSource(source)); adapter.Poll(0);
    assert(adapter.Process(Event::System(EventType::Start,source,1)));
    adapter.Poll(1000 + 24 * 20833 + 100);
    for (uint32_t i = 0; i <= 24; ++i)
        assert(adapter.Process(Event::System(EventType::TimingClock,source,1000 + i * 20833)));
    adapter.Poll(1000 + 24 * 20833 + 100);
    Near(Position(clock),96 + clock.TicksPerUs() * 100);
    const double published = Position(clock);
    assert(adapter.Process(Event::System(EventType::TimingClock,source,1000 + 25 * 20833)));
    assert(Position(clock) >= published);
}

static void LatePulsePhaseRecovery() {
    const SourceId source{0,1};
    MusicalClock clock; ClockFollower follower(clock); ClockAdapter adapter(follower,clock);
    assert(adapter.SelectSource(source)); adapter.Poll(0);
    assert(adapter.Process(Event::System(EventType::Start,source,1)));
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t stamp = 1000 + i * 20833;
        assert(adapter.Process(Event::System(EventType::TimingClock,source,stamp))); adapter.Poll(stamp);
    }
    const uint32_t previous = 1000 + 2 * 20833;
    adapter.Poll(previous + 25000);
    const double published = Position(clock); assert(published > 12);
    assert(adapter.Process(Event::System(EventType::TimingClock,source,previous + 30000)));
    adapter.Poll(previous + 30000); Near(Position(clock),published);
    adapter.Poll(previous + 31000); Near(Position(clock),published);
    const double recovered = 12 + clock.TicksPerUs() * 10000;
    adapter.Poll(previous + 40000); Near(Position(clock),recovered);
    assert(Position(clock) > published);
}

static void TempoChangesAndJitter() {
    const SourceId source{0,1};
    MusicalClock regular_clock, batch_clock;
    ClockFollower regular_follower(regular_clock), batch_follower(batch_clock);
    ClockAdapter regular(regular_follower,regular_clock), batch(batch_follower,batch_clock);
    assert(regular.SelectSource(source) && batch.SelectSource(source));
    regular.Poll(0); batch.Poll(0);
    assert(regular.Process(Event::System(EventType::Start,source,1)));
    assert(batch.Process(Event::System(EventType::Start,source,1)));
    uint32_t stamp = 1000, poll = 0, index = 0;
    auto pulse = [&](uint32_t period) {
        if (index) stamp += period;
        while (poll + 1000 < stamp) { poll += 1000; regular.Poll(poll); }
        const double published = Position(regular_clock);
        const auto event = Event::System(EventType::TimingClock,source,stamp);
        assert(regular.Process(event) && batch.Process(event));
        regular.Poll(stamp); poll = stamp;
        const double received_position = double(index) * 4;
        const double expected = published > received_position ? published : received_position;
        Near(Position(regular_clock),expected);
        Near(Position(batch_clock),received_position);
        assert(regular_clock.Bpm() >= 0 && regular_clock.Bpm() <= kClockBpmMax);
        ++index;
    };
    for (unsigned i = 0; i < 48; ++i) pulse(20833);
    for (unsigned i = 0; i < 96; ++i) pulse(41667);
    Near(regular_follower.Bpm(),60,0.001); Near(batch_follower.Bpm(),60,0.001);
    for (unsigned i = 0; i < 96; ++i) pulse(10000);
    Near(regular_follower.Bpm(),250,0.01); Near(batch_follower.Bpm(),250,0.01);
    for (unsigned i = 0; i < 192; ++i) pulse(i % 2 ? 16167 : 17167);
    Near(regular_follower.Bpm(),150,1); Near(batch_follower.Bpm(),150,1);
    assert(regular_follower.Locked() && batch_follower.Locked());
    // The latest source anchor catches up a briefly ahead interpolated phase.
    regular.Poll(stamp + 10000); batch.Poll(stamp + 10000);
    Near(Position(regular_clock),Position(batch_clock));
}

static void LossPolicyRecovery() {
    const SourceId source{0,1};
    for (const auto policy : {ClockLossPolicy::Freewheel, ClockLossPolicy::Freeze, ClockLossPolicy::Stop}) {
        for (const auto transport : {TransportPolicy::Follow, TransportPolicy::ClockOnly}) {
            MusicalClock clock; ClockFollower follower(clock); ClockAdapter adapter(follower,clock);
            assert(adapter.SelectSource(source)); adapter.SetLossPolicy(policy); adapter.SetTransportPolicy(transport);
            adapter.Poll(0);
            if (transport == TransportPolicy::Follow)
                assert(adapter.Process(Event::System(EventType::Start,source,1)));
            uint32_t last = 0;
            for (uint32_t i = 0; i < 24; ++i) {
                last = 1000 + i * 20833;
                assert(adapter.Process(Event::System(EventType::TimingClock,source,last))); adapter.Poll(last);
            }
            assert(follower.Locked());
            unsigned losses = 0;
            for (uint32_t now = last + 1000; now < last + 2000000; now += 1000) {
                adapter.Poll(now); losses += bool(clock.Events() & CLK_EV::EXT_LOST);
            }
            assert(losses == 1 && !follower.Locked());
            const double retained = Position(clock);
            double resumed = 0;
            for (uint32_t i = 0; i < 12; ++i) {
                const uint32_t stamp = last + 2000000 + i * 20833;
                assert(adapter.Process(Event::System(EventType::TimingClock,source,stamp))); adapter.Poll(stamp);
                if (i == 0) resumed = Position(clock);
                if (policy == ClockLossPolicy::Stop && transport == TransportPolicy::Follow) {
                    Near(Position(clock),retained); assert(!clock.Running());
                } else if (policy == ClockLossPolicy::Freeze && i < 2) Near(Position(clock),retained);
                else Near(Position(clock),resumed + i * 4);
            }
            assert(follower.Locked());
            if (policy == ClockLossPolicy::Freewheel) assert(Position(clock) > retained + 43);
            if (policy == ClockLossPolicy::Stop && transport == TransportPolicy::Follow) {
                const uint32_t next = last + 2000000 + 12 * 20833;
                assert(adapter.Process(Event::System(EventType::Continue,source,next - 1)));
                assert(adapter.Process(Event::System(EventType::TimingClock,source,next))); adapter.Poll(next);
                Near(Position(clock),retained); assert(clock.Running());
                assert(adapter.Process(Event::System(EventType::TimingClock,source,next + 20833))); adapter.Poll(next + 20833);
                Near(Position(clock),retained + 4);
            }
        }
    }

    // Reacquire a slower clock without discarding its new pulse intervals.
    MusicalClock clock; ClockFollower follower(clock); ClockAdapter adapter(follower,clock);
    assert(adapter.SelectSource(source)); adapter.Poll(0);
    assert(adapter.Process(Event::System(EventType::Start,source,1)));
    uint32_t last = 0;
    for (uint32_t i = 0; i < 24; ++i) {
        last = 1000 + i * 20833;
        assert(adapter.Process(Event::System(EventType::TimingClock,source,last))); adapter.Poll(last);
    }
    for (uint32_t now = last + 1000; now < last + 2000000; now += 1000) adapter.Poll(now);
    uint32_t poll = last + 1999000;
    double anchor = 0;
    for (uint32_t i = 0; i < 96; ++i) {
        const uint32_t stamp = last + 2000000 + i * 125000;
        while (poll + 1000 < stamp) { poll += 1000; adapter.Poll(poll); }
        assert(adapter.Process(Event::System(EventType::TimingClock,source,stamp))); adapter.Poll(stamp); poll = stamp;
        if (!i) anchor = Position(clock);
    }
    assert(follower.Locked()); Near(follower.Bpm(),20,0.001);
    Near(Position(clock),anchor + 95 * 4);
}

static void RecoveryTimestamps() {
    SourceId source{0,1};
    MusicalClock clock; ClockFollower follower(clock); ClockAdapter adapter(follower,clock);
    assert(adapter.SelectSource(source)); adapter.Poll(0x90000000u);
    assert(adapter.Process(Event::System(EventType::Start,source,0x90000001u)));
    uint32_t last = 0;
    for (uint32_t i = 0; i < 24; ++i) {
        last = 0x90001000u + i * 20833;
        assert(adapter.Process(Event::System(EventType::TimingClock,source,last))); adapter.Poll(last);
    }
    auto recovery = Event::System(EventType::SourceReset,source);
    recovery.timestamp_valid = false;
    assert(adapter.Process(recovery)); assert(clock.LastTickUs() == last && !clock.Running());
    source.session = 2; assert(adapter.SelectSource(source));
    assert(adapter.Process(Event::System(EventType::Start,source,last + 1)));
    for (uint32_t i = 0; i < 24; ++i) {
        const uint32_t stamp = last + 1000 + i * 20833;
        assert(adapter.Process(Event::System(EventType::TimingClock,source,stamp))); adapter.Poll(stamp);
    }
    Near(Position(clock),92); assert(clock.LastTickUs() != 0 && follower.Locked());
    recovery.source = source;
    const uint32_t resolved = clock.LastTickUs() + 10;
    assert(adapter.Process(recovery,resolved)); assert(clock.LastTickUs() == resolved);

    MusicalClock wrapped_clock; ClockFollower wrapped_follower(wrapped_clock);
    ClockAdapter wrapped(wrapped_follower,wrapped_clock);
    assert(wrapped.SelectSource(source)); wrapped.Poll(0xffff0000u);
    assert(wrapped.Process(Event::System(EventType::Start,source,0xffff0001u)));
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t stamp = 0xffff0100u + i * 20833;
        assert(wrapped.Process(Event::System(EventType::TimingClock,source,stamp))); wrapped.Poll(stamp);
    }
    assert(wrapped.Process(Event::System(EventType::SourceReset,source,0)));
    assert(wrapped_clock.LastTickUs() == 0 && !wrapped_clock.Running());
    source.session = 3; assert(wrapped.SelectSource(source));
    assert(wrapped.Process(Event::System(EventType::Start,source,1)));
    assert(wrapped.Process(Event::System(EventType::TimingClock,source,1000))); wrapped.Poll(1000);
    assert(wrapped_clock.LastTickUs() == 1000 && wrapped_clock.Running());

    MusicalClock fresh_clock; ClockFollower fresh_follower(fresh_clock);
    ClockAdapter fresh(fresh_follower,fresh_clock);
    assert(fresh.SelectSource(source)); recovery.source = source;
    assert(fresh.Process(recovery)); fresh.Poll(0x90000000u);
    assert(fresh_clock.LastTickUs() == 0x90000000u && !fresh_clock.Running());
    auto malformed = Event::System(EventType::Start,source,0); malformed.timestamp_valid = false;
    assert(!fresh.Process(malformed,0x90000001u));
}

static void SynchronizationEvents() {
    struct Grid { uint32_t ppqn; uint8_t beats; };
    constexpr Grid grids[] = {{24,3}, {96,4}, {192,7}};
    for (const auto grid : grids) {
        const auto ppqn = grid.ppqn; const auto beats = grid.beats;
        for (const uint32_t distance : {1u, 2u, 24u, 96u, 256u, 1000u, 5000u}) {
            MusicalClock regular(ppqn,beats), synchronized(ppqn,beats);
            regular.Seek(17); synchronized.Seek(17);
            regular.SetTicksPerUs(1); synchronized.SetTicksPerUs(1);
            regular.Start(); synchronized.Start();
            regular.Tick(0); synchronized.Tick(0);
            regular.Tick(0); synchronized.Tick(0);
            uint32_t events = 0;
            for (uint32_t i = 1; i <= distance; ++i) { regular.Tick(i); events |= regular.Events(); }
            assert(synchronized.Synchronize(17 + distance,0));
            assert(synchronized.Events() == events);
            assert(synchronized.Bar() == regular.Bar() && synchronized.Beat() == regular.Beat()
                && synchronized.TickInBeat() == regular.TickInBeat());
        }
    }
    MusicalClock clock; clock.SetBpm(120); clock.Start(); clock.Tick(0);
    assert(clock.Synchronize(96,0));
    assert(clock.MasterTick() == 96 && (clock.Events() & CLK_EV::QUARTER) && (clock.Events() & CLK_EV::MIDI_CLOCK));
    assert(!clock.Synchronize(95,0)); assert(!clock.Synchronize(96,0,NAN));
    assert(!clock.Synchronize(96,0,1));
    assert(clock.Synchronize(96,0,0.5)); assert(!clock.Synchronize(96,0,0.4));
    clock.Tick(1000); assert(Position(clock) > 96.5);
    clock.Seek(408); clock.Tick(2000);
    assert(clock.MasterTick() == 408 && !(clock.Events() & CLK_EV::QUARTER));

    clock.Seek(std::numeric_limits<uint64_t>::max() - 2);
    clock.Tick(2000);
    clock.SetTicksPerUs(1'000'000'000.25);
    clock.Tick(2001);
    assert(clock.MasterTick() == 999'999'997 && clock.FracTick() == 0.25);
    assert((clock.Events() & CLK_EV::EIGHT_BAR) && (clock.Events() & CLK_EV::MIDI_CLOCK));
}

static void SourceAndTransportPolicies() {
    MusicalClock clock;
    ClockFollower follower(clock);
    ClockAdapter midi(follower,clock);
    const SourceId source{0,1};
    assert(!midi.SelectSource({0,0}));
    assert(midi.SelectSource(source));
    midi.Poll(0);
    assert(!clock.Running());
    assert(!midi.Process(Event::System(EventType::Start,{1,1},1)));
    auto malformed = Event::System(EventType::Start,source,1);
    malformed.note_off_origin = static_cast<NoteOffOrigin>(255);
    assert(!midi.Process(malformed));
    assert(midi.Process(Event::System(EventType::Start,source,1)));
    midi.Poll(1);
    assert(!clock.Running() && !(clock.Events() & (CLK_EV::RESET|CLK_EV::START)));
    for(unsigned i=0;i<48;++i) {
        const uint32_t stamp=1000+i*20833;
        assert(midi.Process(Event::System(EventType::TimingClock,source,stamp)));
        midi.Poll(stamp);
        if(i==0) assert(clock.Running() && clock.MasterTick()==0 && (clock.Events() & CLK_EV::RESET) && (clock.Events() & CLK_EV::START));
    }
    assert(follower.Locked());
    assert(midi.SelectSource(source) && follower.Locked());
    assert(std::fabs(follower.Bpm()-120)<0.1);
    midi.Process(Event::System(EventType::Stop,source,1000000)); midi.Poll(1000000);
    assert(!clock.Running());
    midi.Process(Event::System(EventType::SongPosition,source,1001000,17));
    assert(clock.MasterTick()!=408);
    midi.Poll(1001000);
    assert(clock.MasterTick()==408 && clock.TickInBeat()==24 && clock.Beat()==0 && clock.Bar()==1);
    assert((clock.Events() & (CLK_EV::RESET|CLK_EV::QUARTER))==0);
    midi.Process(Event::System(EventType::Continue,source,1002000)); midi.Poll(1002000);
    assert(!clock.Running() && clock.MasterTick()==408);
    midi.Process(Event::System(EventType::TimingClock,source,1002500)); midi.Poll(1002500);
    assert(clock.Running() && clock.MasterTick()==408);
    midi.SetTransportPolicy(TransportPolicy::ClockOnly);
    assert(!midi.Process(Event::System(EventType::Stop,source,1003000)));
    midi.Poll(1003000); assert(clock.Running());
    midi.Process(Event::System(EventType::SourceReset,source,1004000)); midi.Poll(1004000);
    assert(!clock.Running() && !follower.Locked() && midi.Enabled());
    midi.SetTransportPolicy(TransportPolicy::Follow);
    assert(midi.Process(Event::System(EventType::Start,source,1004100)));
    midi.Poll(1004100);assert(!clock.Running());
    assert(midi.Process(Event::System(EventType::TimingClock,source,1004200)));
    midi.Poll(1004200);assert(clock.Running());
    assert(midi.SelectSource({0,2}));
    const SourceId restarted{0,2};
    assert(!midi.SelectSource({0,1}));
    assert(!midi.Process(Event::System(EventType::TimingClock,source,1004300)));
    midi.SetTransportPolicy(TransportPolicy::Follow);
    midi.Process(Event::System(EventType::Start,restarted,1004500));
    midi.Process(Event::System(EventType::Stop,restarted,1004500)); midi.Poll(1004500);
    assert(!clock.Running());
    midi.Process(Event::System(EventType::Stop,restarted,1004600));
    midi.Process(Event::System(EventType::Continue,restarted,1004600)); midi.Poll(1004600);
    assert(!clock.Running());
    midi.Process(Event::System(EventType::TimingClock,restarted,1004700)); midi.Poll(1004700);
    assert(clock.Running());
    midi.Disable(); assert(!follower.Enabled() && !clock.IsSteered());
    clock.Seek(100); clock.Tick(1005000); assert(clock.MasterTick()==100);
    clock.Seek(200); clock.Reset(); clock.Tick(1006000); assert(clock.MasterTick()==0);
    clock.Reset(); clock.Seek(300); clock.Tick(1007000); assert(clock.MasterTick()==300);
    MusicalClock fast_clock;ClockFollower fast_follower(fast_clock);ClockAdapter fast(fast_follower,fast_clock);
    assert(fast.SelectSource(source));fast.Poll(0);
    for(unsigned i=0;i<24;++i) {fast.Process(Event::System(EventType::TimingClock,source,100+i*5000));fast.Poll(100+i*5000);}
    assert(fast_follower.Locked() && !fast_clock.Running() && fast_clock.Bpm()<=kClockBpmMax);
    fast.Process(Event::System(EventType::Continue,source,120000));fast.Poll(120000);
    fast.Process(Event::System(EventType::TimingClock,source,120100));fast.Poll(120100);
    assert(fast_clock.Running() && fast_clock.Bpm()<=kClockBpmMax);
}

static void StoppedTempoAndResetOrdering() {
    const SourceId source{0,1};
    MusicalClock clock; ClockFollower follower(clock); ClockAdapter adapter(follower,clock);
    assert(adapter.SelectSource(source)); adapter.Poll(0);
    assert(adapter.Process(Event::System(EventType::Start,source,1)));
    uint32_t last = 0;
    for (unsigned i=0;i<24;++i) {
        last=1000+i*20833;
        assert(adapter.Process(Event::System(EventType::TimingClock,source,last))); adapter.Poll(last);
    }
    assert(adapter.Process(Event::System(EventType::Stop,source,last+1000))); adapter.Poll(last+1000);
    for (unsigned i=1;i<=24;++i) {
        const uint32_t stamp=last+i*20833;
        assert(adapter.Process(Event::System(EventType::TimingClock,source,stamp))); adapter.Poll(stamp);
        assert(!clock.Running()); Near(clock.Bpm(),follower.Bpm(),1e-4);
    }
    const uint32_t resumed=last+25*20833;
    assert(adapter.Process(Event::System(EventType::Continue,source,resumed-10))); adapter.Poll(resumed-10);
    assert(!clock.Running());
    assert(adapter.Process(Event::System(EventType::TimingClock,source,resumed))); adapter.Poll(resumed);
    assert(clock.Running()); Near(clock.Bpm(),follower.Bpm(),1e-4);
    assert(adapter.Process(Event::System(EventType::SongPosition,source,resumed+1,17)));
    assert(adapter.Process(Event::System(EventType::SystemReset,source,resumed+2))); adapter.Poll(resumed+2);
    assert(!clock.Running() && clock.MasterTick()==0 && !follower.Locked());
    assert(adapter.Process(Event::System(EventType::SongPosition,source,resumed+3,17)));
    assert(adapter.Process(Event::System(EventType::SourceReset,source,resumed+4))); adapter.Poll(resumed+4);
    assert(!clock.Running() && clock.MasterTick()==408 && adapter.Enabled());
}

static void SourceChangeContinue() {
    MusicalClock clock; ClockFollower follower(clock); ClockAdapter adapter(follower, clock);
    clock.SetBpm(120); clock.Start(); clock.Tick(0); clock.Tick(1000);
    const double retained = Position(clock);
    uint32_t stamp = 1001;
    for (const SourceId source : {SourceId{0, 1}, SourceId{1, 1}}) {
        assert(adapter.SelectSource(source));
        assert(adapter.Process(Event::System(EventType::Continue, source, stamp)));
        assert(adapter.Process(Event::System(EventType::TimingClock, source, stamp + 999)));
        adapter.Poll(stamp + 999);
        assert(clock.Running()); Near(Position(clock), retained);
        stamp += 2000;
    }
}

static void MeterChangePosition() {
    MusicalClock clock;
    clock.Seek(384 + 24); clock.Tick(0);
    assert(!clock.Running() && clock.Bar() == 1 && clock.Beat() == 0);
    clock.SetTimeSignature(3);
    assert(clock.Bar() == 1 && clock.Beat() == 1 && clock.TickInBeat() == 24);
    Near(clock.BarPhase(), 1.25 / 3, 1e-7);
    clock.SetTimeSignature(7);
    assert(clock.Bar() == 0 && clock.Beat() == 4 && clock.TickInBeat() == 24);
    clock.Start(); clock.Tick(0);
    clock.SetTimeSignature(4);
    assert(clock.Running() && clock.Bar() == 1 && clock.Beat() == 0);
    Near(Position(clock), 408);
}

static void InvalidRates() {
    MusicalClock clock;
    clock.SetBpm(120); clock.Start(); clock.Tick(0);
    const double retained_rate = clock.TicksPerUs();
    for (const double invalid : {NAN, INFINITY, -INFINITY}) {
        clock.SetBpm(static_cast<float>(invalid));
        assert(clock.TicksPerUs() == retained_rate);
        clock.SetTicksPerUs(invalid);
        assert(clock.TicksPerUs() == retained_rate);
    }
    clock.Tick(1000); Near(Position(clock),retained_rate * 1000);
    clock.SetTicksPerUs(-1); clock.Tick(2000);
    assert(clock.TicksPerUs() == 0); Near(Position(clock),retained_rate * 1000);
    clock.SetBpm(120); clock.Tick(3000); Near(Position(clock),retained_rate * 2000);
    clock.SetTicksPerUs(std::numeric_limits<double>::max()); clock.Tick(4000);
    Near(Position(clock),retained_rate * 2000);
    clock.SetTicksPerUs(1e20); clock.Tick(5000);
    Near(Position(clock),retained_rate * 2000);
    clock.SetBpm(120); clock.Tick(6000); Near(Position(clock),retained_rate * 3000);
}

int main() {
    ExactColdStartAndBatches();
    TimestampedTransport();
    LateColdBatch();
    LatePulsePhaseRecovery();
    TempoChangesAndJitter();
    LossPolicyRecovery();
    RecoveryTimestamps();
    SynchronizationEvents();
    SourceAndTransportPolicies();
    StoppedTempoAndResetOrdering();
    SourceChangeContinue();
    MeterChangePosition();
    InvalidRates();
}
