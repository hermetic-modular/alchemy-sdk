#include "alchemy/midi/note_part.h"
#include "alchemy/midi/part_persistence.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace alchemy::midi;

static constexpr SourceId a{0, 1}, b{1, 1};
static bool Near(float x, float y, float tolerance = 1.e-5f) { return std::fabs(x - y) <= tolerance; }
struct Capture {
    std::vector<NoteAction> actions;
    void operator()(const NoteAction& action) { actions.push_back(action); }
    const NoteAction& Last() const { assert(!actions.empty()); return actions.back(); }
    void Clear() { actions.clear(); }
};

static void Mono() {
    NotePart<1, 8> part;
    PartConfig config;
    config.articulation = ArticulationPolicy::Legato();
    config.slide.trigger = SlideTrigger::KeyOverlap;
    assert(part.Init(config));
    Capture sink;
    part.Process(Event::NoteOn(a, 0, 60, 100), sink);
    assert(sink.Last().attack_amp && sink.Last().attack_filter && sink.Last().onset);
    assert(!part.Init(config) && part.Gate());
    assert(!sink.Last().pitch_transition.slide && sink.Last().gate);
    auto first = sink.Last().voice;
    part.Process(Event::NoteOn(a, 0, 64, 80), sink);
    assert(sink.Last().kind == ActionKind::Retarget && sink.Last().pitch_transition.slide);
    assert(!sink.Last().attack_amp && !sink.Last().attack_filter && sink.Last().pitch_semitones == 64);
    assert(!(sink.Last().voice == first));
    part.Process(Event::NoteOff(a, 0, 64), sink);
    assert(sink.Last().pitch_semitones == 60 && sink.Last().pitch_transition.slide && !sink.Last().attack_amp);
    part.Process(Event::NoteOff(a, 0, 60, 55), sink);
    const auto release = sink.Last();
    assert(release.kind == ActionKind::Release && !release.gate && Near(release.release_velocity, 55.f / 127.f));
    part.Process(Event::NoteOn(a, 0, 67, 100), sink);
    assert(sink.Last().attack_amp && !sink.Last().pitch_transition.slide);
    assert(!part.NotifyIdle(release.voice));
    assert(part.Gate());
    part.Process(Event::NoteOff(a, 0, 67), sink);
    assert(part.NotifyIdle(sink.Last().voice));
    assert(!part.Gate() && !part.SoundingCount() && !part.TrackedCount());

    config.priority = NotePriority::Low;
    assert(part.ApplyConfig(config, sink));
    part.Process(Event::NoteOn(a, 0, 60, 100), sink);
    sink.Clear();
    part.Process(Event::NoteOn(a, 0, 64, 100), sink);
    assert(sink.actions.empty());
    part.Process(Event::NoteOff(a, 0, 60), sink);
    assert(sink.Last().pitch_semitones == 64 && !sink.Last().attack_amp);
    config.priority = NotePriority::High;
    part.Reset(sink);
    assert(part.ApplyConfig(config, sink));
    part.Process(Event::NoteOn(a, 0, 64, 100), sink);
    sink.Clear();
    part.Process(Event::NoteOn(a, 0, 60, 100), sink);
    assert(sink.actions.empty());
    part.Process(Event::NoteOff(a, 0, 64), sink);
    assert(sink.Last().pitch_semitones == 60);
    part.Reset(sink);
    part.Process(Event::NoteOn(b, 0, 60, 100), sink);
    part.Process(Event::NoteOn(a, 0, 64, 100), sink);
    sink.Clear(); part.ResetSource(a, sink);
    assert(sink.actions.size() == 1 && sink.Last().kind == ActionKind::Retarget);
    assert(sink.Last().note.source == b && sink.Last().pitch_semitones == 60.f && sink.Last().gate);
    assert(!sink.Last().attack_amp && !sink.Last().attack_filter && !sink.Last().onset);
    assert(part.Gate() && part.TrackedCount() == 1);
}

static void DuplicatesAndSources() {
    NotePart<3, 16> part;
    PartConfig config; config.mode = NoteMode::Poly; config.listen = Listen::Omni();
    assert(part.Init(config));
    Capture sink;
    part.Process(Event::NoteOn(a, 0, 60, 100), sink); const auto one = sink.Last();
    part.Process(Event::NoteOn(a, 0, 60, 80), sink); const auto two = sink.Last();
    part.Process(Event::NoteOn(b, 0, 60, 70), sink); const auto other = sink.Last();
    assert(one.voice.slot != two.voice.slot && one.note.occurrence != two.note.occurrence);
    part.Process(Event::NoteOff(a, 0, 60), sink);
    assert(sink.Last().voice == one.voice && sink.Last().kind == ActionKind::Release);
    part.Process(Event::NoteOff(a, 0, 60), sink);
    assert(sink.Last().voice == two.voice);
    part.Process(Event::System(EventType::SourceReset, a), sink);
    assert(part.SoundingCount() == 1 && part.Gate());
    part.Process(Event::NoteOff(b, 0, 60), sink);
    assert(sink.Last().voice == other.voice);
    part.Reset(sink);

    config.duplicates = DuplicatePolicy::Lifo;
    assert(part.ApplyConfig(config, sink));
    part.Process(Event::NoteOn(a, 0, 60, 100), sink); const auto old = sink.Last();
    part.Process(Event::NoteOn(a, 0, 60, 80), sink); const auto young = sink.Last();
    part.Process(Event::NoteOff(a, 0, 60), sink); assert(sink.Last().voice == young.voice);
    part.Process(Event::NoteOff(a, 0, 60), sink); assert(sink.Last().voice == old.voice);

    config.duplicates = DuplicatePolicy::RetriggerSingle;
    assert(part.ApplyConfig(config, sink));
    part.Process(Event::NoteOn(a, 0, 60, 100), sink); const auto single = sink.Last();
    part.Process(Event::NoteOn(a, 0, 60, 80), sink);
    assert(part.TrackedCount() == 1 && sink.Last().voice.slot == single.voice.slot);
    assert(sink.Last().voice.generation != single.voice.generation);
    part.Process(Event::NoteOn(a, 0, 60, 0), sink);
    assert(!part.Gate() && part.TrackedCount() == 0);
}

static void Pedals() {
    NotePart<4, 16> part;
    PartConfig config; config.mode = NoteMode::Poly;
    assert(part.Init(config));
    Capture sink;
    part.Process(Event::Cc(a, 0, 64, 63), sink);
    part.Process(Event::NoteOn(a, 0, 60, 100), sink);
    part.Process(Event::Cc(a, 0, 66, 127), sink);
    part.Process(Event::NoteOn(a, 0, 64, 100), sink);
    part.Process(Event::NoteOff(a, 0, 60), sink);
    part.Process(Event::NoteOff(a, 0, 64), sink);
    assert(part.Gate() && part.TrackedCount() == 1);
    part.Process(Event::Cc(a, 0, 64, 127), sink);
    part.Process(Event::Cc(a, 0, 66, 0), sink);
    assert(part.Gate());
    part.Process(Event::Cc(a, 0, 64, 0), sink);
    assert(!part.Gate());
    part.Process(Event::Cc(a, 0, 64, 127), sink);
    part.Process(Event::NoteOn(a, 0, 67, 100), sink);
    part.Process(Event::NoteOff(a, 0, 67), sink);
    assert(part.Gate());
    part.Process(Event::Cc(a, 0, 123, 0), sink);
    assert(part.Gate());
    part.Process(Event::Cc(a, 0, 121, 0), sink);
    assert(!part.Gate());
    part.Process(Event::Cc(a, 0, 120, 0), sink);
    assert(!part.SoundingCount() && !part.TrackedCount());

    config.mode = NoteMode::Mono; config.articulation = ArticulationPolicy::Legato();
    config.slide.trigger = SlideTrigger::KeyOverlap;
    assert(part.ApplyConfig(config, sink));
    part.Process(Event::Cc(a, 0, 64, 127), sink);
    part.Process(Event::NoteOn(a, 0, 60, 100), sink);
    part.Process(Event::NoteOff(a, 0, 60), sink);
    part.Process(Event::NoteOn(a, 0, 64, 100), sink);
    assert(sink.Last().attack_amp && !sink.Last().pitch_transition.slide);
    part.Process(Event::NoteOff(a, 0, 64), sink);
    part.Process(Event::Cc(a, 0, 64, 0), sink);
    assert(!part.Gate());
}

static void MonoBulkRelease() {
    for (uint8_t controller : {uint8_t(64), uint8_t(66)}) {
        NotePart<1,128> part; PartConfig config; Capture sink;
        assert(part.Init(config));
        if (controller == 64) part.Process(Event::Cc(a,0,controller,127),sink);
        for (unsigned key=0;key<128;++key) part.Process(Event::NoteOn(a,0,uint8_t(key),100),sink);
        if (controller == 66) part.Process(Event::Cc(a,0,controller,127),sink);
        for (unsigned key=0;key<128;++key) part.Process(Event::NoteOff(a,0,uint8_t(key)),sink);
        assert(part.Gate() && part.TrackedCount()==128);
        sink.Clear(); part.Process(Event::Cc(a,0,controller,0),sink);
        assert(!part.Gate() && !part.TrackedCount());
        assert(sink.actions.size()==1 && sink.Last().kind==ActionKind::Release);
        for (unsigned key=0;key<128;++key) part.Process(Event::NoteOn(a,0,uint8_t(key),100),sink);
        sink.Clear(); part.Process(Event::Cc(a,0,123,0),sink);
        assert(!part.Gate() && !part.TrackedCount());
        assert(sink.actions.size()==1 && sink.Last().kind==ActionKind::Release);
    }
}

static void AllocationAndCapacity() {
    NotePart<2, 8> part;
    PartConfig config; config.mode = NoteMode::Poly; config.listen = Listen::Omni();
    assert(part.Init(config));
    Capture sink;
    part.Process(Event::NoteOn(a, 0, 60, 100), sink); const auto first = sink.Last();
    part.Process(Event::NoteOn(a, 0, 62, 100), sink); const auto second = sink.Last();
    part.Process(Event::NoteOff(a, 0, 62), sink);
    part.Process(Event::NoteOn(a, 0, 64, 100), sink); const auto replacement = sink.Last();
    assert(replacement.voice.slot == second.voice.slot);
    assert(!part.NotifyIdle(second.voice));
    sink.Clear(); part.Process(Event::NoteOff(a, 0, 62), sink); assert(sink.actions.empty());
    part.Process(Event::NoteOn(a, 0, 65, 100), sink);
    assert(sink.Last().voice.slot == first.voice.slot);
    sink.Clear(); part.Process(Event::NoteOff(a, 0, 60), sink); assert(sink.actions.empty());
    assert(part.TrackedCount() == 2);
    config.steal = StealPolicy::None;
    part.Reset(sink);
    assert(part.ApplyConfig(config, sink));
    part.Process(Event::NoteOn(a, 0, 60, 100), sink);
    part.Process(Event::NoteOn(a, 0, 62, 100), sink);
    sink.Clear(); part.Process(Event::NoteOn(a, 0, 64, 100), sink);
    assert(sink.actions.empty() && part.TrackedCount() == 3);
    part.Process(Event::NoteOff(a, 0, 64), sink); assert(sink.actions.empty());

    NotePart<1, 1> bounded;
    assert(bounded.Init());
    bounded.Process(Event::NoteOn(a, 0, 60, 100), sink);
    assert(!bounded.Process(Event::NoteOn(a, 0, 62, 100), sink));
    assert(bounded.Faulted(a) && bounded.CapacityFailures() == 1 && !bounded.Gate());
    sink.Clear(); assert(!bounded.Process(Event::NoteOn(a, 0, 64, 100), sink));
    assert(sink.actions.empty());
    bounded.Process(Event::System(EventType::SourceReset, a), sink);
    assert(!bounded.Faulted(a));
    bounded.Process(Event::NoteOn(a, 0, 65, 100), sink); assert(bounded.Gate());

    NotePart<1, 4, 1> contexts;
    assert(contexts.Init(config));
    contexts.Process(Event::NoteOn(a, 0, 60, 100), sink);
    assert(!contexts.Process(Event::NoteOn(b, 0, 62, 100), sink));
    assert(!contexts.Faulted(b) && contexts.Gate() && contexts.CapacityFailures() == 1);
    const SourceId third{2, 1};
    assert(!contexts.Process(Event::NoteOn(third, 0, 64, 100), sink));
    assert(contexts.Gate() && contexts.TrackedCount() == 1);
    assert(!contexts.Process(Event::NoteOn(a, 1, 62, 100), sink));
    assert(contexts.Faulted(a) && !contexts.Gate());
    assert(!contexts.Process(Event::NoteOn(b, 0, 62, 100), sink));
    contexts.ResetSource(b, sink);
    assert(contexts.Faulted(a));
    contexts.ResetSource(a, sink);
    assert(contexts.Process(Event::NoteOn(third, 0, 64, 100), sink));
    assert(contexts.Gate());

    config.steal = StealPolicy::Oldest;
    part.Reset(sink);
    assert(part.ApplyConfig(config, sink));
    part.Process(Event::NoteOn(a, 0, 60, 100), sink);
    part.Process(Event::NoteOn(a, 0, 61, 100), sink);
    part.Process(Event::NoteOff(a, 0, 60), sink);
    sink.Clear(); part.Process(Event::NoteOn(a, 0, 62, 100), sink);
    assert(sink.actions.front().kind == ActionKind::Kill && sink.actions.front().note.key == 60);

    contexts.Reset(sink);
    Event ignored; ignored.source = b; ignored.channel = 15; ignored.type = EventType::ProgramChange;
    assert(!contexts.Process(ignored, sink));
    ignored.type = EventType::PolyPressure; ignored.data1 = 60; ignored.value = 100;
    contexts.Process(ignored, sink);
    contexts.Process(Event::NoteOff(b, 15, 60), sink);
    contexts.Process(Event::Cc(b, 15, 1, 127), sink);
    assert(contexts.Process(Event::NoteOn(a, 0, 60, 100), sink) && contexts.Gate());
}

static void Para() {
    NotePart<3, 16> part;
    PartConfig config; config.mode = NoteMode::Paraphonic;
    config.shared_filter_attack = AttackPolicy::EachSelectedNote;
    config.shared_velocity = SharedVelocity::Maximum;
    assert(part.Init(config));
    Capture sink;
    part.Process(Event::NoteOn(a, 0, 60, 100), sink);
    assert(sink.Last().kind == ActionKind::Group && sink.Last().attack_amp && sink.Last().attack_filter);
    part.Process(Event::NoteOn(a, 0, 64, 80), sink);
    assert(!sink.Last().attack_amp && sink.Last().attack_filter && Near(sink.Last().velocity, 100.f / 127.f));
    assert(!sink.Last().onset);
    part.Process(Event::NoteOff(a, 0, 60), sink);
    assert(part.Gate() && Near(sink.Last().velocity, 80.f / 127.f));
    part.Process(Event::NoteOff(a, 0, 64), sink);
    const auto group = sink.Last();
    assert(!part.Gate() && group.kind == ActionKind::Group && !group.gate);
    assert(Near(group.velocity, 80.f / 127.f));
    assert(part.NotifyGroupIdle(group.group_generation));
    part.Process(Event::NoteOn(a, 0, 67, 100), sink);
    part.Process(Event::NoteOff(a, 0, 67), sink); const auto old = sink.Last().group_generation;
    part.Process(Event::NoteOn(a, 0, 69, 100), sink);
    assert(!part.NotifyGroupIdle(old) && part.Gate());
    part.Reset(sink); assert(!part.Gate() && !part.SoundingCount());

    part.Process(Event::NoteOn(a, 0, 60, 127), sink);
    part.Process(Event::NoteOn(b, 0, 64, 32), sink);
    sink.Clear(); part.ResetSource(a, sink);
    assert(part.Gate() && sink.Last().kind == ActionKind::Group);
    assert(Near(sink.Last().velocity, 32.f / 127.f) && !sink.Last().attack_amp);
    part.Reset(sink);

    NotePart<1, 4> single;
    config.steal = StealPolicy::None;
    assert(single.Init(config));
    single.Process(Event::NoteOn(a, 0, 60, 100), sink);
    single.Process(Event::NoteOff(a, 0, 60), sink);
    const auto tail_generation = sink.Last().group_generation;
    sink.Clear(); single.ResetSource(b, sink);
    assert(sink.actions.empty() && single.SoundingCount() == 1);
    assert(single.NotifyGroupIdle(tail_generation));
    assert(single.Process(Event::NoteOn(a, 0, 64, 100), sink) && single.Gate());
    single.Process(Event::NoteOff(a, 0, 64), sink);
    sink.Clear(); single.ResetSource(a, sink);
    assert(sink.Last().kind == ActionKind::Group && sink.Last().kill && !single.SoundingCount());
    assert(single.Process(Event::NoteOn(a, 0, 67, 100), sink) && single.Gate());
}

static void PolyphonicSlides() {
    NotePart<1, 8> part;
    PartConfig config; config.mode = NoteMode::Poly;
    config.slide.trigger = SlideTrigger::KeyOverlap;
    assert(part.Init(config));
    Capture sink;
    part.Process(Event::NoteOn(a, 0, 60, 100), sink);
    assert(!sink.Last().pitch_transition.slide);
    sink.Clear(); part.Process(Event::NoteOn(a, 0, 64, 100), sink);
    assert(sink.actions.size() == 2 && sink.actions[0].kind == ActionKind::Kill && sink.actions[0].retain_pitch);
    assert(sink.Last().pitch_transition.slide && sink.Last().attack_amp && sink.Last().attack_filter);

    config.slide.trigger = SlideTrigger::Always;
    part.Reset(sink);
    assert(part.ApplyConfig(config, sink));
    part.Process(Event::NoteOn(a, 0, 60, 100), sink);
    assert(!sink.Last().pitch_transition.slide);
    part.Process(Event::NoteOff(a, 0, 60), sink);
    sink.Clear(); part.Process(Event::NoteOn(a, 0, 65, 100), sink);
    assert(sink.actions[0].retain_pitch && sink.Last().pitch_transition.slide);
    part.Process(Event::NoteOff(a, 0, 65), sink);
    assert(part.NotifyIdle(sink.Last().voice));
    part.Process(Event::NoteOn(a, 0, 67, 100), sink);
    assert(!sink.Last().pitch_transition.slide);

    config.mode = NoteMode::Paraphonic; config.slide.trigger = SlideTrigger::KeyOverlap;
    assert(part.ApplyConfig(config, sink));
    sink.Clear(); part.Process(Event::NoteOn(a, 0, 60, 100), sink);
    assert(!sink.actions[0].pitch_transition.slide);
    sink.Clear(); part.Process(Event::NoteOn(a, 0, 64, 100), sink);
    assert(sink.actions[0].kind == ActionKind::Kill && sink.actions[0].retain_pitch);
    assert(sink.actions[1].kind == ActionKind::Assign && sink.actions[1].pitch_transition.slide);

    config.slide.trigger = SlideTrigger::Always;
    part.Reset(sink);
    assert(part.ApplyConfig(config, sink));
    part.Process(Event::NoteOn(a, 0, 60, 100), sink);
    part.Process(Event::NoteOff(a, 0, 60), sink);
    sink.Clear(); part.Process(Event::NoteOn(a, 0, 65, 100), sink);
    assert(sink.actions[0].kind == ActionKind::Kill && sink.actions[0].retain_pitch);
    assert(sink.actions[1].kind == ActionKind::Assign && sink.actions[1].pitch_transition.slide);
}

template <std::size_t Voices>
static void ReleasedGroupReuse() {
    for (auto allocation : {Allocation::RoundRobinFree, Allocation::LowestFree}) {
        NotePart<Voices, 8> part;
        PartConfig config; config.mode = NoteMode::Paraphonic;
        config.slide.trigger = SlideTrigger::Always; config.steal = StealPolicy::None;
        config.allocation = allocation;
        assert(part.Init(config));
        Capture sink;
        std::array<VoiceHandle, Voices> handles{};
        for (uint8_t i = 0; i < Voices; ++i) {
            part.Process(Event::NoteOn(a, 0, static_cast<uint8_t>(60 + i), 100), sink);
            handles[i] = sink.actions[sink.actions.size() - 2].voice;
        }
        for (uint8_t i = 1; i < Voices; ++i)
            part.Process(Event::NoteOff(a, 0, static_cast<uint8_t>(60 + i)), sink);
        part.Process(Event::NoteOff(a, 0, 60), sink);
        const auto generation = sink.Last().group_generation;
        assert(!part.Gate());
        sink.Clear(); part.Process(Event::NoteOn(a, 0, 72, 100), sink);
        assert(sink.actions[0].kind == ActionKind::Kill && sink.actions[0].voice.slot == 0 && sink.actions[0].retain_pitch);
        assert(sink.actions[1].kind == ActionKind::Assign && sink.actions[1].voice.slot == 0 && sink.actions[1].pitch_transition.slide);
        assert(part.Gate() && !part.NotifyGroupIdle(generation) && !part.NotifyIdle(handles[0]));
        for (uint8_t i = 1; i < Voices; ++i) assert(part.NotifyIdle(handles[i]));
    }
}

static void ReleasedGroupFreshSlot() {
    NotePart<3, 8> part;
    PartConfig config; config.mode = NoteMode::Paraphonic;
    config.slide.trigger = SlideTrigger::Always; config.steal = StealPolicy::None;
    assert(part.Init(config));
    Capture sink;
    part.Process(Event::NoteOn(a, 0, 60, 100), sink);
    part.Process(Event::NoteOn(a, 0, 64, 100), sink);
    part.Process(Event::NoteOff(a, 0, 64), sink);
    part.Process(Event::NoteOff(a, 0, 60), sink);
    sink.Clear(); part.Process(Event::NoteOn(a, 0, 72, 100), sink);
    assert(sink.actions[0].kind == ActionKind::Kill && !sink.actions[0].retain_pitch);
    assert(sink.actions[1].kind == ActionKind::Assign && sink.actions[1].voice.slot == 2 && !sink.actions[1].pitch_transition.slide);
}

static void LivePolicies() {
    NotePart<1, 8> part;
    PartConfig config; config.articulation = ArticulationPolicy::Legato();
    config.slide.trigger = SlideTrigger::KeyOverlap;
    assert(part.Init(config));
    Capture sink;
    part.Process(Event::NoteOn(a, 0, 60, 100), sink);
    const auto held = sink.Last().voice;
    config.slide.transition.time_ms = 123.f;
    config.articulation.amp = AttackPolicy::EachSelectedNote;
    config.articulation.filter = AttackPolicy::Never;
    config.articulation.onset = OnsetPolicy::EveryAttack;
    config.allocation = Allocation::LowestFree; config.steal = StealPolicy::Quietest;
    config.duplicates = DuplicatePolicy::Lifo; config.high_resolution_velocity = false;
    sink.Clear(); assert(part.ApplyConfig(config, sink));
    assert(sink.actions.empty() && part.Gate() && part.TrackedCount() == 1);
    assert(part.ReportLevel(held, .25f));
    auto invalid = config; invalid.slide.transition.time_ms = std::numeric_limits<float>::quiet_NaN();
    assert(!part.ApplyConfig(invalid, sink) && sink.actions.empty() && part.ReportLevel(held, .25f));
    part.Process(Event::NoteOn(a, 0, 64, 80), sink);
    assert(sink.Last().pitch_transition.slide && sink.Last().pitch_transition.time_ms == 123.f);
    assert(sink.Last().attack_amp && !sink.Last().attack_filter && sink.Last().onset);
    const auto higher = sink.Last().voice;
    config.priority = NotePriority::Low;
    sink.Clear(); assert(part.ApplyConfig(config, sink));
    assert(sink.actions.size() == 1 && sink.Last().kind == ActionKind::Retarget && sink.Last().pitch_semitones == 60.f);
    assert(sink.Last().gate && !sink.Last().attack_amp && !sink.Last().attack_filter && !sink.Last().onset);
    assert(!part.ReportLevel(higher, .5f));
    config.priority = NotePriority::High;
    sink.Clear(); assert(part.ApplyConfig(config, sink));
    assert(sink.actions.size() == 1 && sink.Last().pitch_semitones == 64.f && !sink.Last().attack_amp);
    const auto same = sink.Last().voice;
    config.slide.transition.time_ms = 200.f;
    sink.Clear(); assert(part.ApplyConfig(config, sink));
    assert(sink.actions.empty() && part.ReportLevel(same, .5f) && part.Gate());
    config.high_resolution_velocity = true; assert(part.ApplyConfig(config, sink));
    part.Process(Event::Cc(a, 0, 88, 33), sink);
    config.high_resolution_velocity = false; assert(part.ApplyConfig(config, sink));
    config.high_resolution_velocity = true; assert(part.ApplyConfig(config, sink));
    part.Process(Event::NoteOn(a, 0, 67, 100), sink);
    assert(Near(sink.Last().velocity, 100.f / 127.f));
    config.listen = Listen::Channel(2);
    sink.Clear(); assert(part.ApplyConfig(config, sink));
    assert(sink.actions.size() == 1 && sink.Last().kind == ActionKind::Kill && !part.Gate() && !part.TrackedCount());
    part.Process(Event::NoteOn(a, 1, 60, 100), sink);
    config.mode = NoteMode::Poly;
    sink.Clear(); assert(part.ApplyConfig(config, sink));
    assert(sink.actions.size() == 1 && sink.Last().kill && !part.SoundingCount());
    part.Process(Event::NoteOn(a, 1, 60, 100), sink);
    config.bend_range = 3.f;
    sink.Clear(); assert(part.ApplyConfig(config, sink));
    assert(sink.actions.size() == 1 && sink.Last().kill && !part.Gate());

    NotePart<3, 8> para;
    config = {}; config.mode = NoteMode::Paraphonic;
    assert(para.Init(config));
    para.Process(Event::NoteOn(a, 0, 60, 40), sink);
    const auto first = sink.actions[sink.actions.size() - 2].voice;
    para.Process(Event::NoteOn(a, 0, 64, 100), sink);
    const auto generation = sink.Last().group_generation;
    assert(Near(sink.Last().velocity, 40.f / 127.f));
    config.shared_velocity = SharedVelocity::Maximum;
    sink.Clear(); assert(para.ApplyConfig(config, sink));
    assert(sink.actions.size() == 1 && Near(sink.Last().velocity, 100.f / 127.f));
    assert(sink.Last().group_generation == generation && sink.Last().gate && !sink.Last().attack_amp && !sink.Last().attack_filter);
    assert(para.ReportLevel(first, .5f) && para.TrackedCount() == 2);
    config.shared_velocity = SharedVelocity::PhraseFirst;
    sink.Clear(); assert(para.ApplyConfig(config, sink));
    assert(sink.actions.size() == 1 && Near(sink.Last().velocity, 40.f / 127.f));
    config.shared_velocity = SharedVelocity::LastAttack;
    sink.Clear(); assert(para.ApplyConfig(config, sink));
    assert(sink.actions.size() == 1 && Near(sink.Last().velocity, 100.f / 127.f));
    config.shared_velocity = SharedVelocity::Fixed; config.fixed_velocity = .75f;
    sink.Clear(); assert(para.ApplyConfig(config, sink));
    assert(sink.actions.size() == 1 && sink.Last().velocity == .75f);
    config.fixed_velocity = .25f;
    sink.Clear(); assert(para.ApplyConfig(config, sink));
    assert(sink.actions.size() == 1 && sink.Last().velocity == .25f);
    para.Process(Event::NoteOff(a, 0, 64), sink);
    para.Process(Event::NoteOff(a, 0, 60), sink);
    assert(!para.Gate());
    config.shared_velocity = SharedVelocity::Maximum;
    sink.Clear(); assert(para.ApplyConfig(config, sink));
    assert(sink.actions.size() == 1 && !sink.Last().gate && !sink.Last().kill && !sink.Last().onset);
    assert(Near(sink.Last().velocity, 40.f / 127.f) && sink.Last().group_generation == generation);
    assert(para.NotifyGroupIdle(generation));
}

static Event Pressure(SourceId source, uint8_t channel, uint8_t value) {
    Event e; e.source = source; e.channel = channel; e.value = value; e.type = EventType::ChannelPressure; return e;
}

static void ExpressionAndMpe() {
    NotePart<4, 16, 32, 2> part;
    PartConfig config; config.mode = NoteMode::Poly; config.enable_mpe = true;
    assert(part.Init(config));
    Capture sink;
    assert(part.ConfigureMpe(a, MpeZone::Lower, 7, sink));
    part.Process(Event::Bend(a, 1, 16383), sink);
    part.Process(Pressure(a, 1, 90), sink);
    part.Process(Event::NoteOn(a, 1, 60, 100), sink);
    assert(Near(sink.Last().expression.bend_semitones, 48.f));
    assert(Near(sink.Last().expression.pressure, 90.f / 127.f));
    const auto first = sink.Last().voice;
    part.Process(Event::Cc(a, 0, 64, 127), sink);
    part.Process(Event::NoteOff(a, 1, 60), sink); assert(part.Gate());
    part.Process(Event::Bend(a, 1, 0), sink);
    assert(Near(sink.Last().expression.bend_semitones, 48.f));
    part.Process(Event::NoteOn(a, 1, 64, 100), sink);
    assert(Near(sink.Last().expression.bend_semitones, -48.f));
    part.Process(Event::Bend(a, 0, 16383), sink);
    assert(Near(sink.Last().expression.zone_bend_semitones, 2.f));
    for (const auto& action : sink.actions) if (action.kind == ActionKind::Expression && action.voice == first)
        assert(Near(action.expression.bend_semitones, 48.f));
    part.Process(Event::Cc(a, 1, 101, 0), sink);
    part.Process(Event::Cc(a, 1, 100, 0), sink);
    part.Process(Event::Cc(a, 1, 6, 12), sink);
    part.Process(Event::NoteOn(a, 2, 67, 100), sink);
    part.Process(Event::Bend(a, 2, 16383), sink);
    assert(Near(sink.Last().expression.bend_semitones, 12.f));
    part.Process(Event::Cc(a, 0, 64, 0), sink);
    assert(part.TrackedCount() == 2);
    assert(part.ConfigureMpe(a, MpeZone::Upper, 15, sink));
    assert(!part.Gate() && !part.SoundingCount());

    MpeState<1> mpe;
    auto update = mpe.Configure(a, MpeZone::Lower, 7);
    assert(update.reset_channels == 0xff);
    assert(mpe.Role(a, 0) == MpeRole::Manager && mpe.Role(a, 7) == MpeRole::Member);
    mpe.Configure(a, MpeZone::Upper, 7);
    assert(mpe.Role(a, 8) == MpeRole::Member && mpe.Role(a, 15) == MpeRole::Manager);
    mpe.Configure(a, MpeZone::Lower, 10);
    assert(mpe.ManagerChannel(a, 10) == 0 && mpe.ManagerChannel(a, 11) == 15);
    assert(mpe.Configure(b, MpeZone::Lower, 1).capacity_exceeded);
    mpe.Process(Event::System(EventType::SystemReset, a));
    assert(mpe.Role(a, 0) == MpeRole::None);
    Event ignored; ignored.source = b; ignored.type = EventType::ProgramChange;
    mpe.Process(ignored);
    assert(!mpe.Configure(a, MpeZone::Lower, 1).capacity_exceeded);

    config.mode = NoteMode::Mono; config.slide.trigger = SlideTrigger::Controller;
    assert(part.ApplyConfig(config, sink));
    assert(part.ConfigureMpe(a, MpeZone::Lower, 2, sink));
    part.Process(Event::NoteOn(a, 1, 60, 100), sink);
    part.Process(Event::Cc(a, 0, 65, 127), sink);
    part.Process(Event::NoteOn(a, 1, 64, 100), sink);
    assert(sink.Last().pitch_transition.slide);
    part.Process(Event::Cc(a, 0, 65, 0), sink);
    part.Process(Event::NoteOn(a, 1, 67, 100), sink);
    assert(!sink.Last().pitch_transition.slide);
    for (uint8_t cc : {124, 125}) for (uint8_t channel : {0, 1, 2}) {
        sink.Clear(); part.Process(Event::Cc(a, channel, cc, 0), sink);
        assert(sink.actions.empty() && part.Gate());
    }
    for (uint8_t channel : {0, 2}) {
        sink.Clear(); part.Process(Event::Cc(a, channel, 127, 0), sink);
        assert(sink.actions.empty() && part.Gate());
    }
    part.Process(Event::Cc(a, 1, 127, 0), sink);
    assert(!part.Gate() && part.Config().mode == NoteMode::Mono);

    NotePart<2, 8, 4, 1> admission;
    assert(admission.Init(config));
    assert(!admission.Process(Event::NoteOn(b, 12, 60, 100), sink));
    assert(!admission.Process(Event::Bend(b, 12, 16383), sink));
    assert(admission.ConfigureMpe(a, MpeZone::Lower, 2, sink));
    assert(!admission.Faulted(a));
    assert(admission.Process(Event::Bend(b, 0, 16383), sink));
    assert(admission.Process(Pressure(b, 0, 100), sink));
    assert(admission.Process(Event::NoteOn(b, 0, 67, 100), sink));
    assert(Near(sink.Last().expression.bend_semitones, 2.f));
    assert(Near(sink.Last().expression.pressure, 100.f / 127.f) && !admission.Faulted(b));
    admission.Process(Pressure(b, 0, 50), sink);
    assert(Near(sink.Last().expression.pressure, 50.f / 127.f));
    admission.Process(Event::Cc(b, 0, 101, 0), sink);
    admission.Process(Event::Cc(b, 0, 100, 0), sink);
    assert(admission.Process(Event::Cc(b, 0, 6, 12), sink));
    assert(Near(sink.Last().expression.bend_semitones, 12.f) && !admission.Faulted(b));
    admission.Process(Event::Cc(b, 0, 64, 127), sink);
    admission.Process(Event::NoteOff(b, 0, 67), sink);
    assert(admission.Gate());
    admission.Process(Event::Cc(b, 0, 64, 0), sink);
    assert(!admission.Gate());
    admission.Reset(sink);
    admission.Process(Event::Cc(a, 15, 101, 0), sink);
    admission.Process(Event::Cc(a, 15, 100, 6), sink);
    assert(admission.Process(Event::Cc(a, 15, 6, 2), sink));
    assert(admission.Process(Event::NoteOn(a, 14, 60, 100), sink) && admission.Gate());
    assert(admission.Process(Event::Cc(a, 15, 6, 3), sink));
    assert(admission.Process(Event::NoteOn(a, 12, 64, 100), sink));
    assert(admission.Process(Event::Cc(a, 15, 6, 0), sink) && !admission.Gate());
    assert(!admission.Process(Event::NoteOn(a, 14, 60, 100), sink));
    assert(admission.Process(Event::Cc(a, 15, 6, 2), sink));
    assert(admission.Process(Event::NoteOn(a, 14, 60, 100), sink) && admission.Gate());
    config.listen = Listen::Channel(1, a.port);
    assert(admission.ApplyConfig(config, sink));
    assert(!admission.ConfigureMpe(b, MpeZone::Lower, 2, sink));
    assert(admission.ConfigureMpe(a, MpeZone::Lower, 2, sink));
    admission.Process(Event::Cc(a, 0, 101, 0), sink);
    admission.Process(Event::Cc(a, 0, 100, 6), sink);
    assert(admission.ConfigureMpe(a, MpeZone::Lower, 0, sink));
    assert(admission.Process(Event::Cc(a, 0, 6, 3), sink));
    assert(admission.Process(Event::NoteOn(a, 3, 60, 100), sink) && admission.Gate());
    MpeState<1> repeated;
    repeated.Process(Event::Cc(a, 0, 101, 0));
    repeated.Process(Event::Cc(a, 0, 100, 6));
    repeated.Process(Event::Cc(a, 0, 6, 2));
    repeated.Process(Event::Cc(a, 0, 6, 3));
    assert(repeated.Role(a, 3) == MpeRole::Member);
    repeated.Process(Event::Cc(a, 0, 6, 0));
    assert(repeated.Role(a, 3) == MpeRole::None);
    repeated.Process(Event::Cc(a, 0, 6, 2));
    assert(repeated.Role(a, 2) == MpeRole::Member);

    NotePart<1, 8> ordinary;
    assert(ordinary.Init());
    ordinary.Process(Event::Bend(a, 0, 16383), sink);
    ordinary.Process(Event::Cc(a, 0, 101, 0), sink);
    ordinary.Process(Event::Cc(a, 0, 100, 0), sink);
    ordinary.Process(Event::Cc(a, 0, 6, 7), sink);
    ordinary.Process(Event::Cc(a, 0, 38, 25), sink);
    ordinary.Process(Event::NoteOn(a, 0, 60, 100), sink);
    assert(Near(sink.Last().expression.bend_semitones, 7.25f));
    ordinary.Process(Event::NoteOff(a, 0, 60), sink);
    ordinary.Process(Event::Bend(a, 0, 0), sink);
    assert(Near(sink.Last().expression.bend_semitones, -7.25f));
    ordinary.Process(Event::Cc(a, 0, 6, 12), sink);
    assert(Near(sink.Last().expression.bend_semitones, -12.f));

    PartConfig legato; legato.articulation = ArticulationPolicy::Legato();
    assert(ordinary.ApplyConfig(legato, sink));
    ordinary.Process(Event::NoteOn(a, 0, 60, 100), sink);
    ordinary.Process(Event::NoteOn(a, 0, 64, 100), sink);
    Event pressure; pressure.type = EventType::PolyPressure; pressure.source = a;
    pressure.channel = 0; pressure.data1 = 60; pressure.value = 90;
    ordinary.Process(pressure, sink);
    ordinary.Process(Event::NoteOff(a, 0, 64), sink);
    assert(Near(sink.Last().expression.key_pressure, 90.f / 127.f));
    ordinary.Process(Event::Cc(a, 0, 121, 0), sink);
    assert(sink.Last().expression.key_pressure == 0.f);

    MpeState<1> plain;
    plain.Process(Event::Bend(a, 7, 16383));
    plain.Process(Pressure(a, 7, 100));
    assert(plain.Expression(a, 7).pressure > 0.f);
    plain.Process(Event::Cc(a, 7, 121, 0));
    assert(plain.Expression(a, 7).pressure == 0.f && plain.Expression(a, 7).bend_semitones == 0.f);
}

static void HighResolutionVelocityAndLifecycle() {
    NotePart<3, 16> part;
    PartConfig config; config.mode = NoteMode::Poly; config.listen = Listen::Omni();
    assert(part.Init(config));
    Capture sink;
    part.Process(Event::Cc(a, 0, 88, 17), sink);
    part.Process(Event::NoteOn(b, 0, 60, 100), sink);
    assert(Near(sink.Last().velocity, 100.f / 127.f));
    part.Process(Event::NoteOn(a, 1, 60, 100), sink);
    assert(Near(sink.Last().velocity, 100.f / 127.f));
    part.Process(Event::NoteOn(a, 0, 60, 100), sink);
    assert(Near(sink.Last().velocity, (12800.f + 17.f) / 16383.f));
    const auto handle = sink.Last().voice;
    part.Process(Event::Cc(a, 0, 88, 33), sink);
    part.Process(Event::NoteOff(a, 0, 60, 60), sink);
    assert(Near(sink.Last().release_velocity, (7680.f + 33.f) / 16383.f));
    part.Process(Event::NoteOn(a, 0, 62, 100), sink);
    assert(Near(sink.Last().velocity, 100.f / 127.f));
    assert(!part.NotifyIdle(handle));
    part.Process(Event::Cc(a, 0, 88, 127), sink);
    const auto tracked = part.TrackedCount();
    part.Process(Event::NoteOn(a, 0, 65, 0), sink);
    assert(part.TrackedCount() == tracked);
    part.Process(Event::NoteOn(a, 0, 67, 100), sink);
    assert(Near(sink.Last().velocity, 100.f / 127.f));
    part.Process(Event::Cc(a, 0, 88, 127), sink);
    part.Process(Event::NoteOn(a, 0, 67, 0), sink);
    assert(sink.Last().kind == ActionKind::Release && sink.Last().release_velocity == 0.f);
    part.Process(Event::NoteOn(a, 0, 67, 100), sink);
    part.Process(Event::Cc(a, 0, 88, 17), sink);
    part.Process(Event::NoteOff(a, 0, 67, 0), sink);
    assert(Near(sink.Last().release_velocity, 17.f / 16383.f));
    Event high = Event::NoteOn(a, 0, 70, 100); high.value = 10001; high.resolution = 14;
    part.Process(Event::Cc(a, 0, 88, 127), sink);
    part.Process(high, sink); assert(Near(sink.Last().velocity, 10001.f / 16383.f));
    part.Process(Event::NoteOn(a, 0, 71, 100), sink);
    assert(Near(sink.Last().velocity, 100.f / 127.f));
    const auto before_reset = sink.Last().voice;
    part.Reset(sink);
    assert(part.Init(config));
    part.Process(Event::NoteOn(a, 0, 69, 100), sink);
    assert(!part.NotifyIdle(before_reset));
    const SourceId wrapped{0, std::numeric_limits<uint32_t>::max()};
    part.Process(Event::NoteOn(wrapped, 0, 72, 100, 0xfffffff0u), sink);
    part.Process(Event::System(EventType::SourceReset, a, 16u), sink);
    assert(part.Gate() && part.TrackedCount() == 1);
    part.Process(Event::Cc(wrapped, 0, 126, 1), sink);
    assert(!part.Gate() && part.Config().mode == NoteMode::Poly);
    assert(part.Config().listen.channel_mask == 0xffffu);
}

static const NoteAction& LastVoiceAction(const Capture& sink, ActionKind kind) {
    for (auto it = sink.actions.rbegin(); it != sink.actions.rend(); ++it)
        if (it->kind == kind) return *it;
    assert(false); return sink.Last();
}

static void ResetBothPedals() {
    for (const auto mode : {NoteMode::Mono, NoteMode::Poly, NoteMode::Paraphonic}) {
        NotePart<3, 8> part;
        PartConfig config; config.mode = mode;
        config.articulation = ArticulationPolicy::Legato(); config.slide.trigger = SlideTrigger::Controller;
        assert(part.Init(config));
        Capture sink;
        part.Process(Event::Cc(a, 0, 74, 90), sink);
        part.Process(Event::Cc(a, 0, 101, 0), sink);
        part.Process(Event::Cc(a, 0, 100, 0), sink);
        part.Process(Event::Cc(a, 0, 6, 12), sink);
        part.Process(Event::Bend(a, 0, 16383), sink);
        part.Process(Pressure(a, 0, 80), sink);
        part.Process(Event::Cc(a, 0, 65, 127), sink);
        part.Process(Event::NoteOn(a, 0, 60, 100), sink);
        part.Process(Event::Cc(a, 0, 66, 127), sink);
        part.Process(Event::Cc(a, 0, 64, 127), sink);
        part.Process(Event::NoteOn(a, 0, 64, 80), sink);
        const auto current = LastVoiceAction(sink, mode == NoteMode::Mono ? ActionKind::Retarget : ActionKind::Assign).voice;
        part.Process(Event::NoteOff(a, 0, 60, 40), sink);
        part.Process(Event::NoteOff(a, 0, 64, 30), sink);
        part.Process(Event::Cc(a, 0, 88, 33), sink);
        sink.Clear(); part.Process(Event::Cc(a, 0, 121, 0), sink);
        assert(!part.Gate() && !part.TrackedCount());
        unsigned releases = 0;
        for (const auto& action : sink.actions) {
            assert(action.kind != ActionKind::Assign && action.kind != ActionKind::Retarget);
            if (action.kind == ActionKind::Release) {
                ++releases;
                if (mode == NoteMode::Mono) {
                    assert(action.voice == current && action.note.key == 64);
                    assert(Near(action.release_velocity, 30.f / 127.f));
                }
            }
        }
        assert(releases == (mode == NoteMode::Mono ? 1u : 2u));
        part.Process(Event::NoteOn(a, 0, 67, 100), sink);
        const auto& assigned = LastVoiceAction(sink, mode == NoteMode::Mono ? ActionKind::Retarget : ActionKind::Assign);
        assert(!assigned.pitch_transition.slide && Near(assigned.velocity, 100.f / 127.f));
        assert(assigned.expression.bend_semitones == 0.f && assigned.expression.pressure == 0.f);
        assert(Near(assigned.expression.timbre, 90.f / 127.f));
        part.Process(Event::Bend(a, 0, 16383), sink);
        assert(Near(sink.Last().expression.bend_semitones, 12.f));
        part.Process(Event::NoteOff(a, 0, 67), sink); assert(!part.Gate());
    }
}

static void AllSoundOffOwnershipAndControllers() {
    for (auto mode : {NoteMode::Mono, NoteMode::Poly, NoteMode::Paraphonic})
        for (auto duplicates : {DuplicatePolicy::Fifo, DuplicatePolicy::Lifo, DuplicatePolicy::RetriggerSingle}) {
            NotePart<2, 16> part;
            PartConfig config; config.mode = mode; config.duplicates = duplicates;
            config.articulation = ArticulationPolicy::Legato();
            config.slide.trigger = SlideTrigger::KeyOverlap;
            assert(part.Init(config));
            Capture sink;
            part.Process(Event::NoteOn(a, 0, 60, 100), sink);
            part.Process(Event::Cc(a, 0, 64, 127), sink);
            part.Process(Event::Cc(a, 0, 66, 127), sink);
            part.Process(Event::Cc(a, 0, 65, 127), sink);
            part.Process(Event::Bend(a, 0, 16383), sink);
            part.Process(Pressure(a, 0, 89), sink);
            part.Process(Event::Cc(a, 0, 74, 102), sink);
            part.Process(Event::Cc(a, 0, 101, 0), sink);
            part.Process(Event::Cc(a, 0, 100, 0), sink);
            part.Process(Event::Cc(a, 0, 6, 12), sink);
            part.Process(Event::Cc(a, 0, 120, 0), sink);
            assert(!part.Gate() && !part.SoundingCount() && part.TrackedCount() == 1);
            // Pedal release must not discard the silent pending NoteOff.
            part.Process(Event::Cc(a, 0, 66, 0), sink);
            assert(part.TrackedCount() == 1);
            sink.Clear(); part.Process(Event::NoteOn(a, 0, 60, 80), sink);
            const auto assigned = LastVoiceAction(sink, ActionKind::Assign);
            assert(!assigned.pitch_transition.slide && part.Gate() && part.TrackedCount() == 2);
            assert(Near(assigned.expression.bend_semitones, 12.f));
            assert(Near(assigned.expression.pressure, 89.f / 127.f));
            assert(Near(assigned.expression.timbre, 102.f / 127.f));
            if (mode == NoteMode::Mono) assert(assigned.attack_amp && assigned.onset);
            part.Process(Event::Cc(a, 0, 6, 7), sink);
            assert(Near(LastVoiceAction(sink, ActionKind::Expression).expression.bend_semitones, 7.f));
            sink.Clear(); part.Process(Event::NoteOff(a, 0, 60), sink);
            assert(sink.actions.empty() && part.Gate() && part.TrackedCount() == 1);
            part.Process(Event::NoteOff(a, 0, 60, 55), sink);
            assert(part.Gate()); // The original damper state still applies.
            part.Process(Event::Cc(a, 0, 64, 0), sink);
            const auto release = LastVoiceAction(sink, ActionKind::Release);
            assert(release.voice == assigned.voice && Near(release.release_velocity, 55.f / 127.f));
            assert(release.note_off_origin == NoteOffOrigin::Explicit && !part.Gate() && !part.TrackedCount());
            part.Reset(sink);
        }

    for (auto mode : {NoteMode::Mono, NoteMode::Poly, NoteMode::Paraphonic}) {
        NotePart<2, 8> held; PartConfig config; config.mode = mode;
        assert(held.Init(config)); Capture sink;
        held.Process(Event::NoteOn(a, 0, 60, 100), sink);
        held.Process(Event::Cc(a, 0, 66, 127), sink);
        held.Process(Event::Cc(a, 0, 120, 0), sink);
        held.Process(Event::NoteOn(a, 0, 64, 100), sink);
        // A pedal that stayed down must not capture new notes on repeated CC66.
        held.Process(Event::Cc(a, 0, 66, 127), sink);
        held.Process(Event::NoteOff(a, 0, 64), sink);
        assert(!held.Gate() && held.TrackedCount() == 1);
        held.Process(Event::NoteOff(a, 0, 60), sink); assert(!held.TrackedCount());
    }
    {
        NotePart<1, 8> slide; PartConfig config; config.slide.trigger = SlideTrigger::Controller;
        assert(slide.Init(config)); Capture sink;
        slide.Process(Event::Cc(a, 0, 65, 127), sink);
        slide.Process(Event::NoteOn(a, 0, 60, 100), sink);
        slide.Process(Event::Cc(a, 0, 120, 0), sink);
        slide.Process(Event::NoteOn(a, 0, 64, 100), sink);
        slide.Process(Event::NoteOn(a, 0, 67, 100), sink);
        assert(sink.Last().pitch_transition.slide);
    }

    NotePart<4, 16> part;
    PartConfig config; config.mode = NoteMode::Poly; config.listen = Listen::Omni();
    assert(part.Init(config)); Capture sink;
    part.Process(Event::NoteOn(a, 0, 60, 100), sink);
    part.Process(Event::NoteOn(a, 1, 62, 100), sink);
    part.Process(Event::NoteOn(b, 0, 64, 100), sink);
    part.Process(Event::Cc(a, 0, 120, 0), sink);
    assert(part.Gate() && part.SoundingCount() == 2 && part.TrackedCount() == 3);
    sink.Clear(); part.Process(Event::NoteOff(a, 0, 60), sink);
    assert(sink.actions.empty() && part.TrackedCount() == 2);
    part.Process(Event::NoteOff(a, 1, 62), sink);
    assert(part.Gate());
    part.Process(Event::NoteOff(b, 0, 64), sink);
    assert(!part.Gate());

    // A scoped silence may resume an unaffected mono key; Never still suppresses attack.
    config.mode = NoteMode::Mono; config.articulation.amp = AttackPolicy::Never;
    assert(part.ApplyConfig(config, sink));
    part.Process(Event::NoteOn(b, 0, 60, 100), sink);
    part.Process(Event::NoteOn(a, 0, 64, 100), sink);
    sink.Clear(); part.Process(Event::Cc(a, 0, 120, 0), sink);
    assert(sink.actions.size() == 2 && sink.actions[0].kind == ActionKind::Kill);
    assert(sink.Last().kind == ActionKind::Assign && sink.Last().note.source == b);
    assert(sink.Last().gate && !sink.Last().attack_amp);
}

static void MpeAllSoundOff() {
    NotePart<4, 16, 32, 2> part;
    PartConfig config; config.mode = NoteMode::Poly; config.enable_mpe = true;
    config.listen = Listen::Omni(); assert(part.Init(config)); Capture sink;
    assert(part.ConfigureMpe(a, MpeZone::Lower, 2, sink));
    assert(part.ConfigureMpe(b, MpeZone::Upper, 2, sink));
    part.Process(Event::Bend(a, 0, 16383), sink);
    part.Process(Event::Bend(a, 1, 16383), sink);
    part.Process(Event::Cc(a, 0, 74, 87), sink);
    part.Process(Event::Cc(a, 1, 74, 101), sink);
    part.Process(Pressure(a, 0, 33), sink);
    part.Process(Pressure(a, 1, 99), sink);
    part.Process(Event::Cc(a, 0, 64, 127), sink);
    part.Process(Event::NoteOn(a, 1, 60, 100), sink);
    part.Process(Event::NoteOn(b, 14, 67, 100), sink);
    part.Process(Event::Cc(a, 0, 120, 0), sink);
    assert(part.Gate() && part.SoundingCount() == 1 && part.TrackedCount() == 2);
    part.Process(Event::NoteOn(a, 1, 60, 80), sink);
    const auto assigned = sink.Last();
    assert(Near(assigned.expression.bend_semitones, 48.f) && Near(assigned.expression.zone_bend_semitones, 2.f));
    assert(Near(assigned.expression.pressure, 99.f / 127.f) && Near(assigned.expression.zone_pressure, 33.f / 127.f));
    assert(Near(assigned.expression.timbre, 101.f / 127.f) && Near(assigned.expression.zone_timbre, 87.f / 127.f));
    sink.Clear(); part.Process(Event::NoteOff(a, 1, 60), sink);
    assert(sink.actions.empty() && part.TrackedCount() == 2);
    part.Process(Event::NoteOff(a, 1, 60, 70), sink);
    assert(part.TrackedCount() == 2);
    part.Process(Event::Cc(a, 0, 64, 0), sink);
    assert(sink.Last().kind == ActionKind::Release && sink.Last().voice == assigned.voice);
    assert(part.Gate() && part.TrackedCount() == 1);
    part.Process(Event::NoteOff(b, 14, 67), sink); assert(!part.Gate());
}

static void MonoPitchHistory() {
    for (auto trigger : {SlideTrigger::Always, SlideTrigger::Controller, SlideTrigger::KeyOverlap})
        for (bool idle : {false, true}) {
            NotePart<1, 8> part; PartConfig config; config.slide.trigger = trigger;
            assert(part.Init(config)); Capture sink;
            part.Process(Event::Cc(a, 0, 65, 127), sink);
            part.Process(Event::NoteOn(a, 0, 60, 100), sink);
            assert(!sink.Last().pitch_transition.slide);
            part.Process(Event::NoteOff(a, 0, 60), sink);
            if (idle) assert(part.NotifyIdle(sink.Last().voice));
            part.Process(Event::NoteOn(a, 0, 72, 100), sink);
            assert(sink.Last().pitch_transition.slide == (trigger != SlideTrigger::KeyOverlap));
            assert(sink.Last().kind == (idle ? ActionKind::Assign : ActionKind::Retarget));
            part.Process(Event::NoteOff(a, 0, 72), sink);
            assert(part.NotifyIdle(sink.Last().voice));
            part.ResetSource(a, sink);
            part.Process(Event::Cc(a, 0, 65, 127), sink);
            part.Process(Event::NoteOn(a, 0, 67, 100), sink);
            assert(!sink.Last().pitch_transition.slide);
            part.Process(Event::NoteOff(a, 0, 67), sink);
            assert(part.NotifyIdle(sink.Last().voice));
            part.Reset(sink);
            part.Process(Event::Cc(a, 0, 65, 127), sink);
            part.Process(Event::NoteOn(a, 0, 69, 100), sink);
            assert(!sink.Last().pitch_transition.slide);
        }
}

static void ReleaseProvenance() {
    for (auto origin : {NoteOffOrigin::Explicit, NoteOffOrigin::ZeroVelocityNoteOn, NoteOffOrigin::Unknown})
        for (bool pedal : {false, true}) {
            NotePart<1, 8> part; assert(part.Init()); Capture sink;
            if (pedal) part.Process(Event::Cc(a, 0, 64, 127), sink);
            part.Process(Event::NoteOn(a, 0, 60, 100), sink);
            assert(sink.Last().note_off_origin == NoteOffOrigin::Unknown);
            auto off = origin == NoteOffOrigin::ZeroVelocityNoteOn ? Event::NoteOn(a, 0, 60, 0)
                                                                  : Event::NoteOff(a, 0, 60);
            off.note_off_origin = origin;
            part.Process(off, sink);
            if (pedal) part.Process(Event::Cc(a, 0, 64, 0), sink);
            assert(sink.Last().kind == ActionKind::Release && sink.Last().note_off_origin == origin);
            assert(sink.Last().release_velocity == 0.f);
        }
    NotePart<1, 8> part; assert(part.Init()); Capture sink;
    part.Process(Event::NoteOn(a, 0, 60, 100), sink);
    Event shorthand = Event::NoteOn(a, 0, 60, 100); shorthand.value = 0;
    part.Process(shorthand, sink);
    assert(sink.Last().note_off_origin == NoteOffOrigin::ZeroVelocityNoteOn);
    part.Process(Event::NoteOn(a, 0, 60, 100), sink);
    part.Process(Event::Cc(a, 0, 123, 0), sink);
    assert(sink.Last().note_off_origin == NoteOffOrigin::Unknown);
}

static void SystemResetAndPartCodec() {
    for (auto scope : {SystemResetScope::Source, SystemResetScope::Part, SystemResetScope::Ignore}) {
        NotePart<2, 8> part; PartConfig config; config.mode = NoteMode::Poly;
        config.system_reset_scope = scope; config.listen = Listen::Omni();
        assert(part.Init(config)); Capture sink;
        part.Process(Event::NoteOn(a, 0, 60, 100), sink);
        part.Process(Event::NoteOn(b, 0, 64, 100), sink);
        sink.Clear(); const bool accepted = part.Process(Event::System(EventType::SystemReset, a), sink);
        assert(accepted == (scope != SystemResetScope::Ignore));
        const size_t remaining = scope == SystemResetScope::Part ? 0 : scope == SystemResetScope::Source ? 1 : 2;
        assert(part.TrackedCount() == remaining && part.SoundingCount() == remaining);
        if (scope == SystemResetScope::Ignore) assert(sink.actions.empty());
        std::array<uint8_t, PartCodec::kSize> bytes{};
        assert(PartCodec::Encode(config, bytes.data(), bytes.size()));
        assert(bytes[4] == 2 && !(bytes[34] & 16));
        PartConfig restored; assert(PartCodec::Decode(bytes.data(), bytes.size(), restored));
        assert(restored.system_reset_scope == scope);
    }
    NotePart<1, 8> part; PartConfig config; config.listen = Listen::Channel(1, a.port);
    assert(config.system_reset_scope == SystemResetScope::Part && part.Init(config)); Capture sink;
    part.Process(Event::NoteOn(a, 0, 60, 100), sink);
    assert(part.Process(Event::System(EventType::SystemReset, b), sink) && !part.SoundingCount());
    config.system_reset_scope = static_cast<SystemResetScope>(255);
    assert(!part.ApplyConfig(config, sink));
    MpeState<2> mpe; mpe.Configure(a, MpeZone::Lower, 2); mpe.Configure(b, MpeZone::Upper, 2);
    mpe.Process(Event::System(EventType::SourceReset, a));
    assert(mpe.Role(a, 0) == MpeRole::None && mpe.Role(b, 15) == MpeRole::Manager);
    mpe.Configure(a, MpeZone::Lower, 2);
    mpe.Process(Event::System(EventType::SystemReset, a));
    assert(mpe.Role(a, 0) == MpeRole::None && mpe.Role(b, 15) == MpeRole::None);
}

static uint32_t Random(uint32_t& state) {
    state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state;
}

static void Randomized() {
    for (bool with_mpe : {false, true}) for (auto mode : {NoteMode::Mono, NoteMode::Poly, NoteMode::Paraphonic}) {
        NotePart<6, 24, 16, 2> part;
        PartConfig config; config.mode = mode; config.listen = Listen::Omni();
        config.enable_mpe = with_mpe;
        assert(part.Init(config));
        uint32_t random = 0x81738137u;
        std::array<uint32_t, 6> generations{};
        Capture sink;
        for (unsigned iteration = 0; iteration < 50000; ++iteration) {
            Event e; e.type = static_cast<EventType>(Random(random) % 25);
            e.source = {(Random(random) % 23 == 0) ? kAnyPort : static_cast<uint16_t>(Random(random) % 3),
                        Random(random) % 31 == 0 ? 0u : 1u};
            e.channel = static_cast<uint8_t>(Random(random) % 20);
            e.data1 = static_cast<uint8_t>(Random(random) % 256);
            e.value = static_cast<uint16_t>(Random(random) % 32768);
            e.time_us = 0xfffffff0u + iteration;
            if (iteration % 4) {
                e.type = static_cast<EventType>(Random(random) % 8);
                e.source = {static_cast<uint16_t>(Random(random) % 3), 1};
                e.channel %= 4; e.data1 %= 128; e.value %= 128;
                if (e.type == EventType::PitchBend) { e.value *= 128; e.resolution = 14; }
            }
            const auto before_notes = part.TrackedCount(), before_voices = part.SoundingCount();
            sink.Clear();
            part.Process(e, sink);
            if (!e.Valid()) {
                assert(sink.actions.empty());
                assert(part.TrackedCount() == before_notes && part.SoundingCount() == before_voices);
            }
            for (const auto& action : sink.actions) {
                assert(std::isfinite(action.velocity) && action.velocity >= 0.f && action.velocity <= 1.f);
                assert(std::isfinite(action.release_velocity) && action.release_velocity >= 0.f && action.release_velocity <= 1.f);
                assert(std::isfinite(action.expression.TotalBend()));
                if (action.kind == ActionKind::Group) assert(action.voice.slot == kNoVoice);
                else {
                    assert(action.voice.slot < 6 && action.voice.generation);
                    if (action.kind == ActionKind::Assign || action.kind == ActionKind::Retarget) {
                        assert(action.voice.generation != generations[action.voice.slot]);
                        generations[action.voice.slot] = action.voice.generation;
                    }
                }
            }
            assert(part.TrackedCount() <= 24 && part.SoundingCount() <= 6);
            if (iteration % 128 == 0) {
                part.ResetSource({static_cast<uint16_t>((iteration / 128) % 3), 1}, sink);
            }
            if (with_mpe && iteration % 512 == 0) {
                part.ResetSource(a, sink); part.ResetSource(b, sink);
                assert(part.ConfigureMpe(a, MpeZone::Lower, 3, sink));
                assert(part.ConfigureMpe(b, MpeZone::Upper, 3, sink));
            }
        }
        part.Reset(sink); assert(!part.Gate() && !part.TrackedCount() && !part.SoundingCount());
    }
}

static void SlewAndArbiter() {
    for (float sample_rate : {48000.f, 96000.f}) {
        PitchSlew slew; assert(slew.Init(sample_rate, 60.f));
        PitchTransition transition; transition.slide = true; transition.time_ms = 10.f;
        assert(slew.SetTarget(72.f, transition));
        const auto frames = static_cast<unsigned>(sample_rate / 100.f);
        float previous = 60.f;
        for (unsigned frame = 0; frame < frames; ++frame) {
            const float current = slew.Process(); assert(current >= previous && current <= 72.f); previous = current;
        }
        assert(slew.Current() == 72.f && !slew.Active());
        assert(slew.SetTarget(60.f, transition));
        for (unsigned i = 0; i < frames / 2; ++i) slew.Process();
        const float mid = slew.Current(); assert(slew.SetTarget(67.f, transition) && slew.Current() == mid);
        transition.slide = false; assert(slew.SetTarget(69.f, transition));
        assert(slew.Current() == 69.f && !slew.Active());
        assert(!slew.SetTarget(std::numeric_limits<float>::quiet_NaN()));
    }
    for (auto curve : {SlideCurve::Linear,SlideCurve::Exponential}) {
        PitchSlew samples,blocks; assert(samples.Init(96000.f,60.f) && blocks.Init(96000.f,60.f));
        PitchTransition transition; transition.slide=true; transition.time_ms=125; transition.curve=curve;
        assert(samples.SetTarget(72.f,transition) && blocks.SetTarget(72.f,transition));
        for (unsigned frame=0;frame<12000;frame+=48) {
            for (unsigned i=0;i<48;++i) samples.Process();
            assert(Near(samples.Current(),blocks.Advance(48)));
        }
        assert(!blocks.Active() && blocks.Advance(0)==72 && blocks.Advance(UINT32_MAX)==72);
        assert(samples.SetTarget(57.f,transition) && blocks.SetTarget(57.f,transition));
        for (unsigned i=0;i<40;++i) {
            for (unsigned j=0;j<24;++j) samples.Process();
            blocks.Advance(23); assert(Near(samples.Current(),blocks.Process()));
        }
        assert(blocks.Advance(UINT32_MAX)==57 && !blocks.Active());
    }
    {
        PitchSlew slew; assert(slew.Init(48000.f, 60.f));
        PitchTransition transition; transition.slide = true; transition.time_ms = 100.f;
        assert(slew.SetTarget(72.f, transition)); slew.Advance(2400);
        assert(slew.Active() && slew.Current() < 72.f);
        transition.time_ms = 0.f;
        assert(slew.SetTarget(72.f, transition) && slew.Current() == 72.f && !slew.Active());
        transition.timing = SlideTiming::Rate; transition.semitones_per_second = 24.f;
        assert(slew.SetTarget(60.f, transition) && slew.Active() && slew.Current() == 72.f);
    }
    assert(Near(NoteToFrequency(69.f), 440.f));
    for (auto curve : {SlideCurve::Linear, SlideCurve::Exponential}) {
        PitchSlew lower, higher; assert(lower.Init(48000.f, 60.f) && higher.Init(96000.f, 60.f));
        PitchTransition transition; transition.slide = true; transition.time_ms = 125.f; transition.curve = curve;
        assert(lower.SetTarget(72.f, transition) && higher.SetTarget(72.f, transition));
        for (unsigned frame = 0; frame < 6000; ++frame) {
            higher.Process(); const float fast = higher.Process(); const float slow = lower.Process();
            assert(Near(fast, slow));
        }
        assert(!lower.Active() && !higher.Active());
    }
    {
        PitchSlew rate; assert(rate.Init(48000.f, 60.f));
        PitchTransition transition; transition.slide = true; transition.timing = SlideTiming::Rate;
        transition.semitones_per_second = 24.f;
        assert(rate.SetTarget(72.f, transition));
        for (unsigned frame = 0; frame < 24000; ++frame) rate.Process();
        assert(rate.Current() == 72.f && !rate.Active());
        transition.time_ms = std::numeric_limits<float>::max();
        transition.timing = SlideTiming::Duration;
        assert(!rate.SetTarget(60.f, transition) && rate.Current() == 72.f);
        assert(rate.SetTarget(std::numeric_limits<float>::max()));
        transition.time_ms = 10.f;
        assert(rate.SetTarget(-std::numeric_limits<float>::max(), transition));
        for (unsigned frame = 0; frame < 480; ++frame) assert(std::isfinite(rate.Process()));
    }
    PitchGateArbiter arbiter;
    PitchGateConfig config; config.gate = GateMerge::Union; config.cv_attacks = true;
    assert(arbiter.Init(config));
    PitchGateInput in; in.midi_gate = true; in.midi_pitch = 60.f; in.cv_transpose = 12.f;
    in.midi_note_on = in.midi_attack_amp = in.midi_attack_filter = true;
    auto out = arbiter.Process(in); assert(out.gate && out.attack_amp && out.pitch == 72.f);
    in.midi_note_on = in.midi_attack_amp = in.midi_attack_filter = false; in.cv_gate = true;
    out = arbiter.Process(in); assert(out.gate && out.attack_amp && out.attack_filter);
    in.midi_gate = false; out = arbiter.Process(in); assert(out.gate && !out.attack_amp);
    in.cv_gate = false; out = arbiter.Process(in); assert(!out.gate);
    config.pitch = PitchMerge::LastActive; config.owner_return_attack = true;
    assert(arbiter.Init(config));
    in.cv_gate = in.midi_gate = in.midi_note_on = true; in.cv_sequence = 9; in.midi_sequence = 10;
    out = arbiter.Process(in); assert(out.owner == PitchOwner::Midi);
    in.midi_gate = in.midi_note_on = false; out = arbiter.Process(in);
    assert(out.owner == PitchOwner::Cv && out.attack_amp);
    config.midi_attacks = config.cv_attacks = false;
    assert(arbiter.Init(config));
    in = {}; in.midi_gate = in.midi_note_on = true;
    out = arbiter.Process(in); assert(!out.attack_amp && out.owner == PitchOwner::Midi);
    in.midi_note_on = false; in.cv_gate = true;
    out = arbiter.Process(in); assert(!out.attack_amp && out.owner == PitchOwner::Cv);
    in.cv_gate = false;
    out = arbiter.Process(in); assert(out.attack_amp && out.owner == PitchOwner::Midi);
    config.pitch = PitchMerge::LastActive; assert(arbiter.Init(config));
    in = {}; in.midi_gate=true; in.midi_pitch=73.f;
    out=arbiter.Process(in); assert(out.owner==PitchOwner::Midi && out.pitch==73.f);
    assert(arbiter.Init(config));
    in = {}; in.cv_gate=true; in.cv_pitch=48.f;
    out=arbiter.Process(in); assert(out.owner==PitchOwner::Cv && out.pitch==48.f);
    config.pitch = PitchMerge::CvPlusMidi; config.midi_reference = std::numeric_limits<float>::max();
    assert(arbiter.Init(config));
    in.cv_pitch = in.midi_pitch = std::numeric_limits<float>::max();
    out = arbiter.Process(in); assert(out.pitch == std::numeric_limits<float>::max());
}

int main() {
    Mono(); DuplicatesAndSources(); Pedals(); MonoBulkRelease(); AllocationAndCapacity(); Para(); PolyphonicSlides();
    ReleasedGroupReuse<2>(); ReleasedGroupReuse<3>(); ReleasedGroupFreshSlot(); LivePolicies(); ExpressionAndMpe();
    HighResolutionVelocityAndLifecycle(); ResetBothPedals(); AllSoundOffOwnershipAndControllers(); MpeAllSoundOff();
    MonoPitchHistory(); ReleaseProvenance(); SystemResetAndPartCodec(); Randomized(); SlewAndArbiter();
    std::puts("MIDI notes, allocation, articulation, MPE, pitch and merge tests passed");
}
