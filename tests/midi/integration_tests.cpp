#include "alchemy/midi.h"
#include <array>
#include <cassert>
#include <cmath>
#include <vector>

using namespace alchemy::midi;

static void ConfigurationPersistence() {
    PartConfig config;
    config.listen = Listen::Channel(16, 2);
    config.mode = NoteMode::Paraphonic; config.priority = NotePriority::High;
    config.duplicates = DuplicatePolicy::Lifo; config.allocation = Allocation::LowestFree;
    config.steal = StealPolicy::Quietest; config.articulation = ArticulationPolicy::Legato();
    config.slide.trigger = SlideTrigger::Controller; config.slide.return_to_held = false;
    config.slide.transition.curve = SlideCurve::Linear; config.slide.transition.timing = SlideTiming::Rate;
    config.slide.transition.time_ms = 45.f; config.slide.transition.semitones_per_second = 16.f;
    config.shared_amp_attack = AttackPolicy::Never; config.shared_filter_attack = AttackPolicy::EachSelectedNote;
    config.shared_velocity = SharedVelocity::Fixed; config.fixed_velocity = 0.75f;
    config.bend_range = 7.25f; config.sustain = false; config.sostenuto = false;
    config.enable_mpe = true; config.high_resolution_velocity = false;
    std::array<uint8_t, PartCodec::kSize> encoded{}, check{};
    assert(PartCodec::Encode(config, encoded.data(), encoded.size()));
    PartConfig restored;
    assert(PartCodec::Decode(encoded.data(), encoded.size(), restored));
    assert((!NotePart<1, 8>::Validate(restored)));
    assert((NotePart<1, 8, 16, 1>::Validate(restored)));
    assert(PartCodec::Encode(restored, check.data(), check.size()) && check == encoded);
    assert(!restored.high_resolution_velocity && restored.listen.channel_mask == 0x8000);
    for (size_t i = 0; i < encoded.size(); ++i) {
        auto corrupt = encoded; corrupt[i] ^= 0x80;
        assert(!PartCodec::Decode(corrupt.data(), corrupt.size(), restored));
        assert(PartCodec::Encode(restored, check.data(), check.size()) && check == encoded);
    }
    PartSerializable serializable(restored);
    restored.bend_range = NAN;
    assert(!serializable.CanSerialize());
    serializable.Serialize(check.data());
    for (auto byte : check) assert(byte == 0);
    assert(!serializable.Deserialize(check.data()));
}

static void MonoOff() {
    MonoOptions options;
    NotePart<1, 8> part;
    std::vector<NoteAction> actions;
    const auto sink = [&](const NoteAction& action) { actions.push_back(action); };
    const auto held = Event::NoteOn({0, 1}, 0, 60, 100);
    assert(part.Init(options.Part()) && part.Process(held, sink));
    auto invalid = options;
    invalid.channel = 255;
    assert(!part.ApplyConfig(invalid.Part(), sink) && part.Gate());

    options.Disable();
    assert(part.ApplyConfig(options.Part(), sink));
    assert(actions.size() == 2 && actions.back().kind == ActionKind::Kill);
    assert(!part.Gate() && part.SoundingCount() == 0 && part.TrackedCount() == 0);
    assert(!part.Process(held, sink) && actions.size() == 2);

    std::array<uint8_t, PartCodec::kSize> bytes{};
    PartConfig restored;
    assert(PartCodec::Encode(options.Part(), bytes.data(), bytes.size()));
    assert(PartCodec::Decode(bytes.data(), bytes.size(), restored));
    assert(part.Init(restored) && !part.Process(held, sink));
    assert(options.SetChannel(2) && part.ApplyConfig(options.Part(), sink));
    assert(!part.Process(held, sink));
    assert(part.Process(Event::NoteOn({0, 1}, 1, 64, 100), sink) && part.Gate());
}

static void NotesAndParameters() {
    NotePart<4, 16> part;
    PartConfig config; config.mode = NoteMode::Poly; config.listen = Listen::Omni();
    assert(part.Init(config));
    ParameterMixer<2, 8> mixer;
    assert(mixer.Register({1, ParameterType::Continuous, 0, 1, 0.5f}));
    Routes<2> routes;
    Route route; route.id = 1; route.target = 1; route.number = 20; route.listen = Listen::Omni();
    route.operation = RouteOp::AddOffset; route.minimum = -0.5f; route.maximum = 0.5f; route.bipolar = true;
    assert(routes.Add(route));
    ControllerDecoder<4> decoder;
    Ingress<8, 2> ingress;
    std::vector<NoteAction> actions;
    const auto sink = [&](const Event& e) {
        part.Process(e, [&](const NoteAction& action) { actions.push_back(action); });
        decoder.Process(e, [&](const ControllerMessage& message) {
            if (message.kind == ControllerKind::Disconnect || (message.kind == ControllerKind::Reset && message.number == 0))
                mixer.ResetSource(message.source);
            else if (message.kind == ControllerKind::Reset) mixer.ResetSource(message.source, message.channel);
            else routes.Process(message, [&](const ParameterCommand& command) { assert(mixer.Apply(command)); });
        });
    };
    assert(ingress.Push(Event::NoteOn({0, 1}, 0, 60, 100)));
    assert(ingress.Push(Event::NoteOn({1, 1}, 0, 67, 100)));
    assert(ingress.Push(Event::Cc({0, 1}, 0, 20, 127)));
    ingress.Drain(sink);
    assert(part.SoundingCount() == 2 && mixer.Value(1) == 1.f);
    assert(ingress.Push(Event::Cc({1, 1}, 0, 20, 0)));
    ingress.Drain(sink);
    assert(mixer.Value(1) == 0.f);
    assert(ingress.Push(Event::System(EventType::SourceReset, {1, 1})));
    ingress.Drain(sink);
    assert(part.SoundingCount() == 1 && mixer.Value(1) == 1.f);
    assert(ingress.Push(Event::Cc({0, 1}, 0, 121, 0)));
    ingress.Drain(sink);
    assert(part.Gate() && mixer.Value(1) == 0.5f);
    assert(ingress.Push(Event::NoteOff({0, 1}, 0, 60)));
    ingress.Drain(sink);
    assert(!part.Gate());
}

int main() { ConfigurationPersistence(); MonoOff(); NotesAndParameters(); }
