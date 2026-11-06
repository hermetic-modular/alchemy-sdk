#pragma once

#include "alchemy/midi/mpe.h"
#include "alchemy/midi/pitch.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace alchemy::midi {

constexpr uint16_t kNoVoice = 0xffffu;

struct NoteToken {
    SourceId source{};
    uint32_t occurrence = 0;
    uint8_t channel = 0, key = 0;
    bool operator==(const NoteToken& other) const {
        return source == other.source && occurrence == other.occurrence &&
               channel == other.channel && key == other.key;
    }
};

struct VoiceHandle {
    uint16_t slot = kNoVoice;
    uint32_t generation = 0;
    bool operator==(VoiceHandle other) const {
        return slot == other.slot && generation == other.generation;
    }
};

enum class NoteMode : uint8_t { Mono, Poly, Paraphonic, Off };
enum class NotePriority : uint8_t { Last, Low, High };
enum class DuplicatePolicy : uint8_t { Fifo, Lifo, RetriggerSingle };
enum class Allocation : uint8_t { RoundRobinFree, LowestFree };
enum class StealPolicy : uint8_t { ReleasedThenSustainedThenHeld, Oldest, Quietest, None };
// Policies refer to the mono part, each newly assigned poly voice, or the
// paraphonic group. Mono fallback to a held key keeps the current envelopes.
enum class AttackPolicy : uint8_t { EachSelectedNote, FirstPhysicalKey, FirstEligibleNote, Never };
enum class OnsetPolicy : uint8_t { FirstPhrase, EveryAttack, Never };
enum class SlideTrigger : uint8_t { Off, KeyOverlap, EligibleOverlap, Always, Controller };
enum class SharedVelocity : uint8_t { PhraseFirst, LastAttack, Maximum, Fixed };
enum class ActionKind : uint8_t { Assign, Retarget, Release, Expression, Kill, Group };
enum class SystemResetScope : uint8_t { Source, Part, Ignore };

struct ArticulationPolicy {
    AttackPolicy amp = AttackPolicy::EachSelectedNote;
    AttackPolicy filter = AttackPolicy::EachSelectedNote;
    OnsetPolicy onset = OnsetPolicy::FirstPhrase;
    static constexpr ArticulationPolicy Retrigger() { return {}; }
    static constexpr ArticulationPolicy Legato() {
        return {AttackPolicy::FirstPhysicalKey, AttackPolicy::FirstPhysicalKey, OnsetPolicy::FirstPhrase};
    }
};

struct SlidePolicy {
    SlideTrigger trigger = SlideTrigger::Off;
    PitchTrajectory transition{};
    bool return_to_held = true;
};

struct PartConfig {
    Listen listen{};
    NoteMode mode = NoteMode::Mono;
    NotePriority priority = NotePriority::Last;
    DuplicatePolicy duplicates = DuplicatePolicy::Fifo;
    Allocation allocation = Allocation::RoundRobinFree;
    StealPolicy steal = StealPolicy::ReleasedThenSustainedThenHeld;
    ArticulationPolicy articulation{};
    SlidePolicy slide{};
    AttackPolicy shared_amp_attack = AttackPolicy::FirstEligibleNote;
    AttackPolicy shared_filter_attack = AttackPolicy::FirstEligibleNote;
    SharedVelocity shared_velocity = SharedVelocity::PhraseFirst;
    float fixed_velocity = 1.f;
    float bend_range = 2.f;
    bool sustain = true, sostenuto = true;
    bool high_resolution_velocity = true;
    // Zone channels and manager RPN6 setup bypass channel_mask; the port filter still applies.
    bool enable_mpe = false;
    // Applies to SystemReset regardless of the listen channel or port.
    SystemResetScope system_reset_scope = SystemResetScope::Part;

    bool Valid() const {
        return (mode == NoteMode::Off || listen.channel_mask) && mode <= NoteMode::Off &&
            priority <= NotePriority::High && duplicates <= DuplicatePolicy::RetriggerSingle &&
            allocation <= Allocation::LowestFree && steal <= StealPolicy::None &&
            articulation.amp <= AttackPolicy::Never && articulation.filter <= AttackPolicy::Never &&
            articulation.onset <= OnsetPolicy::Never && shared_amp_attack <= AttackPolicy::Never &&
            shared_filter_attack <= AttackPolicy::Never && shared_velocity <= SharedVelocity::Fixed &&
            system_reset_scope <= SystemResetScope::Ignore && slide.trigger <= SlideTrigger::Controller &&
            slide.transition.curve <= SlideCurve::Exponential && slide.transition.timing <= SlideTiming::Rate &&
            std::isfinite(bend_range) && bend_range >= 0.f &&
            std::isfinite(fixed_velocity) && fixed_velocity >= 0.f && fixed_velocity <= 1.f &&
            std::isfinite(slide.transition.time_ms) && slide.transition.time_ms >= 0.f &&
            std::isfinite(slide.transition.semitones_per_second) && slide.transition.semitones_per_second > 0.f;
    }
};

struct NoteAction {
    ActionKind kind = ActionKind::Expression;
    VoiceHandle voice{};
    NoteToken note{};
    NoteExpression expression{};
    PitchTransition pitch_transition{};
    float pitch_semitones = 0.f;
    float velocity = 0.f, release_velocity = 0.f;
    // Voice release provenance; Unknown before NoteOff and for channel-mode releases.
    NoteOffOrigin note_off_origin = NoteOffOrigin::Unknown;
    uint32_t group_generation = 0;
    uint16_t group = 0;
    bool gate = false, pitch_changed = false;
    bool attack_amp = false, attack_filter = false, onset = false;
    bool kill = false;
    // A kill may retain the DSP slew position for the following assignment.
    bool retain_pitch = false;
    bool retain_for_group_release = false;
};

template <std::size_t Voices, std::size_t Notes, std::size_t Channels = 32,
          std::size_t MpeSources = 0>
class NotePart {
    static_assert(Voices > 0 && Voices < kNoVoice && Notes > 0 && Notes < kNoVoice);
    static_assert(Channels > 0);
  public:
    NotePart() = default;
    bool Init(PartConfig config = {}) {
        if (!Validate(config) || TrackedCount() || SoundingCount() || group_busy_) return false;
        config_ = config;
        notes_ = {}; contexts_ = {};
        sources_ = {};
        mpe_.Reset();
        for (auto& voice : voices_) voice.pitch_valid = false;
        selected_ = kNoVoice; cursor_ = 0;
        order_ = 0; failures_ = 0;
        group_gate_ = group_busy_ = false; group_velocity_ = 1.f;
        group_first_velocity_ = group_last_velocity_ = group_max_velocity_ = 1.f;
        return true;
    }

    template <class Sink>
    bool ApplyConfig(PartConfig config, Sink&& sink) {
        if (!Validate(config)) return false;
        if (OwnershipChange(config)) {
            Reset(sink);
            config_ = config;
            return true;
        }
        const auto previous = config_;
        config_ = config;
        if (previous.high_resolution_velocity != config.high_resolution_velocity)
            for (auto& context : contexts_) context.velocity_prefix = false;
        if (config_.mode == NoteMode::Mono && previous.priority != config_.priority)
            SelectMono(false, false, false, sink);
        if (config_.mode == NoteMode::Paraphonic && group_busy_ &&
            (previous.shared_velocity != config_.shared_velocity || previous.fixed_velocity != config_.fixed_velocity)) {
            const float velocity = group_velocity_;
            UpdateGroupVelocity();
            if (velocity != group_velocity_) EmitGroup(group_gate_, false, false, false, sink);
        }
        return true;
    }

    const PartConfig& Config() const { return config_; }
    static bool Validate(const PartConfig& config) {
        return config.Valid() && (!config.enable_mpe || MpeSources > 0);
    }
    uint32_t CapacityFailures() const { return failures_; }
    bool Faulted(SourceId source) const {
        for (const auto& state : sources_)
            if (state.used && state.source == source) return state.faulted;
        return false;
    }
    // Includes silenced key-down occurrences awaiting their required NoteOff.
    std::size_t TrackedCount() const {
        std::size_t count = 0;
        for (const auto& n : notes_) count += n.used;
        return count;
    }
    std::size_t SoundingCount() const {
        std::size_t count = 0;
        for (const auto& v : voices_) count += v.state != State::Free;
        return count;
    }
    bool Gate() const {
        if (config_.mode == NoteMode::Paraphonic) return group_gate_;
        for (const auto& v : voices_) if (v.state == State::Held || v.state == State::Pedal) return true;
        return false;
    }

    bool NotifyIdle(VoiceHandle handle) {
        if (handle.slot >= Voices) return false;
        auto& v = voices_[handle.slot];
        if (v.generation != handle.generation || v.state != State::Released || v.group_tail) return false;
        v.state = State::Free;
        v.note_index = kNoVoice;
        return true;
    }
    bool NotifyGroupIdle(uint32_t generation) {
        if (config_.mode != NoteMode::Paraphonic || group_gate_ || !group_busy_ ||
            generation != group_generation_) return false;
        group_busy_ = false;
        for (auto& v : voices_) if (v.group_tail && v.state == State::Released) {
            v.state = State::Free; v.group_tail = false; v.note_index = kNoVoice;
        }
        return true;
    }
    bool ReportLevel(VoiceHandle handle, float level) {
        if (handle.slot >= Voices || !std::isfinite(level) || level < 0.f) return false;
        auto& v = voices_[handle.slot];
        if (v.generation != handle.generation || v.state == State::Free) return false;
        v.level = level;
        return true;
    }

    template <class Sink>
    void Reset(Sink&& sink) {
        for (uint16_t slot = 0; slot < Voices; ++slot) KillVoice(slot, sink);
        notes_ = {}; contexts_ = {}; sources_ = {}; mpe_.Reset(); selected_ = kNoVoice;
        if (group_busy_) EmitGroup(false, false, false, true, sink);
        group_gate_ = group_busy_ = false;
    }

    template <class Sink>
    void ResetSource(SourceId source, Sink&& sink) {
        ResetScope(source, 0xffffu, sink);
        mpe_.Reset(source);
        for (auto& state : sources_) if (state.used && state.source == source) state = Source{};
    }

    template <class Sink>
    bool ConfigureMpe(SourceId source, MpeZone zone, uint8_t members, Sink&& sink) {
        if (!config_.enable_mpe || members > 15 || zone > MpeZone::Upper ||
            source.port == kAnyPort || !source.session || !SourceMatches(source)) return false;
        if (Faulted(source) || !ReserveSource(source)) return false;
        const uint8_t manager = zone == MpeZone::Lower ? 0 : 15;
        const auto* previous = FindContext(source, manager);
        const bool have_selector = previous != nullptr;
        const uint8_t msb = previous ? previous->rpn_msb : 127, lsb = previous ? previous->rpn_lsb : 127;
        const bool rpn = !previous || previous->rpn;
        const auto update = mpe_.Configure(source, zone, members);
        if (update.capacity_exceeded) { Fail(source, sink); return false; }
        if (update.reset_channels) ResetScope(source, update.reset_channels, sink);
        if (have_selector) {
            auto* context = GetContext(source, manager);
            if (!context) { Fail(source, sink); return false; }
            context->rpn_msb = msb; context->rpn_lsb = lsb; context->rpn = rpn;
            mpe_.RestoreSelection(source, manager, msb, lsb, rpn);
        }
        UpdateExpression(source, update.expression_channels, sink);
        return true;
    }

    template <class Sink>
    bool Process(const Event& e, Sink&& sink) {
        if (!e.Valid()) return false;
        if (e.type == EventType::SourceReset) { ResetSource(e.source, sink); return true; }
        if (e.type == EventType::SystemReset) {
            if (config_.system_reset_scope == SystemResetScope::Ignore) return false;
            if (config_.system_reset_scope == SystemResetScope::Part) Reset(sink);
            else ResetSource(e.source, sink);
            return true;
        }
        if (!e.IsChannel() || !SourceMatches(e.source) || Faulted(e.source)) return false;
        const auto existing_role = config_.enable_mpe ? mpe_.Role(e.source, e.channel) : MpeRole::None;
        const bool setup = MpeSetup(e);
        const bool configuration = MpeConfiguration(e);
        if (existing_role == MpeRole::None && !config_.listen.Matches(e) && !setup) return false;
        if (e.type != EventType::NoteOn && e.type != EventType::NoteOff &&
            e.type != EventType::PitchBend && e.type != EventType::ChannelPressure &&
            e.type != EventType::PolyPressure && e.type != EventType::ControlChange) return false;
        if (e.type == EventType::ControlChange && !ControlRelevant(e.data1)) return false;
        const bool needs_context = (e.type == EventType::NoteOn && e.value) || e.type == EventType::PitchBend ||
            e.type == EventType::ChannelPressure || (e.type == EventType::ControlChange && e.data1 < 120);
        if (needs_context && !ReserveSource(e.source)) return false;
        MpeUpdate update;
        if (config_.enable_mpe) {
            if (existing_role != MpeRole::None) update = mpe_.Process(e);
            else if (configuration)
                update = mpe_.Configure(e.source, e.channel == 0 ? MpeZone::Lower : MpeZone::Upper,
                                        static_cast<uint8_t>(e.value));
            if (update.capacity_exceeded) { Fail(e.source, sink); return false; }
            if (update.reset_channels) ResetScope(e.source, update.reset_channels, sink);
        }
        const auto role = config_.enable_mpe ? mpe_.Role(e.source, e.channel) : MpeRole::None;
        if (role == MpeRole::None && !config_.listen.Matches(e) && !setup) return false;
        if (role == MpeRole::Member && e.type == EventType::ControlChange &&
            e.data1 != 74 && e.data1 != 100 && e.data1 != 101 && e.data1 != 6 &&
            e.data1 != 38 && e.data1 != 98 && e.data1 != 99 && e.data1 != 88 && e.data1 < 124)
            return false;
        Context temporary;
        auto* context = needs_context ? GetContext(e.source, e.channel)
                                     : FindContext(e.source, e.channel);
        if (!context && needs_context) { Fail(e.source, sink); return false; }
        if (!context && (e.type == EventType::NoteOff || e.type == EventType::NoteOn)) return true;
        if (!context) context = &temporary;
        if (configuration) {
            context->rpn_msb = 0; context->rpn_lsb = 6; context->rpn = true;
            mpe_.RestoreSelection(e.source, e.channel, 0, 6, true);
        }
        switch (e.type) {
        case EventType::NoteOn:
            if (e.value) NoteOn(e, sink); else NoteOff(e, sink);
            break;
        case EventType::NoteOff: NoteOff(e, sink); break;
        case EventType::PitchBend: context->bend = e.value; UpdateExpression(e.source, Scope(e), sink); break;
        case EventType::ChannelPressure:
            context->pressure = static_cast<uint8_t>(e.value); UpdateExpression(e.source, Scope(e), sink); break;
        case EventType::PolyPressure:
            if (role != MpeRole::Member) {
                for (auto& n : notes_) if (n.used && n.token.source == e.source &&
                    n.token.channel == e.channel && n.token.key == e.data1)
                    n.key_pressure = e.value / 127.f;
                for (auto& v : voices_) if (v.state != State::Free && v.note.source == e.source &&
                    v.note.channel == e.channel && v.note.key == e.data1) {
                    v.key_pressure = e.value / 127.f;
                    EmitExpression(v, sink);
                }
            }
            break;
        case EventType::ControlChange: Control(e, *context, role, sink); break;
        default: break;
        }
        if (update.expression_channels && e.type != EventType::PitchBend &&
            e.type != EventType::ChannelPressure && !(e.type == EventType::ControlChange && e.data1 == 74))
            UpdateExpression(e.source, update.expression_channels, sink);
        return !Faulted(e.source);
    }

  private:
    enum class State : uint8_t { Free, Held, Pedal, Released };
    struct Note {
        NoteToken token{};
        NoteExpression frozen{};
        uint64_t order = 0;
        uint16_t voice = kNoVoice;
        float velocity = 0.f, release_velocity = 0.f;
        NoteOffOrigin note_off_origin = NoteOffOrigin::Unknown;
        float key_pressure = 0.f;
        bool used = false, key_down = false, sustain = false, sostenuto = false;
        bool member_frozen = false, silenced = false;
    };
    struct Voice {
        NoteToken note{};
        NoteExpression expression{};
        uint64_t order = 0, release_order = 0;
        uint32_t generation = 0;
        uint16_t note_index = kNoVoice;
        float velocity = 0.f, release_velocity = 0.f, level = 1.f, key_pressure = 0.f;
        State state = State::Free;
        NoteOffOrigin note_off_origin = NoteOffOrigin::Unknown;
        bool pitch_valid = false;
        bool member_frozen = false, group_tail = false;
    };
    struct Context {
        SourceId source{};
        float range = 2.f;
        uint16_t bend = 8192;
        uint8_t channel = 0, pressure = 0, timbre = 64;
        uint8_t rpn_msb = 127, rpn_lsb = 127, range_semitones = 2, range_cents = 0;
        uint8_t velocity_lsb = 0;
        bool velocity_prefix = false;
        bool used = false, sustain = false, sostenuto = false, portamento = false, rpn = true;
    };
    struct Source {
        SourceId source{};
        bool used = false, faulted = false;
    };

    bool OwnershipChange(const PartConfig& next) const {
        return next.mode != config_.mode || next.listen.channel_mask != config_.listen.channel_mask ||
            next.listen.port != config_.listen.port || next.enable_mpe != config_.enable_mpe ||
            next.sustain != config_.sustain || next.sostenuto != config_.sostenuto ||
            next.bend_range != config_.bend_range;
    }
    bool SourceMatches(SourceId source) const {
        return config_.mode != NoteMode::Off &&
            (config_.listen.port == kAnyPort || config_.listen.port == source.port);
    }
    bool MpeSetup(const Event& e) const {
        if (!config_.enable_mpe || e.type != EventType::ControlChange ||
            (e.channel != 0 && e.channel != 15)) return false;
        if ((e.data1 == 101 && e.value == 0) || (e.data1 == 100 && e.value == 6)) return true;
        const auto* context = FindContext(e.source, e.channel);
        return context && (MpeConfiguration(e) ||
            ((e.data1 == 101 || e.data1 == 100) && e.value == 127));
    }
    bool MpeConfiguration(const Event& e) const {
        if (!config_.enable_mpe || e.type != EventType::ControlChange || e.data1 != 6 ||
            e.value > 15 || (e.channel != 0 && e.channel != 15)) return false;
        const auto* context = FindContext(e.source, e.channel);
        return context && context->rpn && context->rpn_msb == 0 && context->rpn_lsb == 6;
    }
    static uint16_t Bit(uint8_t ch) { return static_cast<uint16_t>(1u << ch); }
    static uint32_t Next(uint32_t value) { return ++value ? value : 1; }
    static bool Eligible(const Note& n) { return n.used && !n.silenced && (n.key_down || n.sustain || n.sostenuto); }
    bool ControlRelevant(uint8_t number) const {
        switch (number) {
        case 64: return config_.sustain;
        case 66: return config_.sostenuto;
        case 88: return config_.high_resolution_velocity;
        case 65: case 74: case 6: case 38: case 98: case 99: case 100: case 101:
        case 120: case 121: case 123: case 124: case 125: case 126: case 127: return true;
        default: return false;
        }
    }
    bool ReserveSource(SourceId source) {
        for (const auto& state : sources_) if (state.used && state.source == source) return true;
        for (auto& state : sources_) if (!state.used) {
            state.source = source; state.used = true; return true;
        }
        if (failures_ != std::numeric_limits<uint32_t>::max()) ++failures_;
        return false;
    }
    Context* GetContext(SourceId source, uint8_t channel) {
        for (auto& c : contexts_) if (c.used && c.source == source && c.channel == channel) return &c;
        for (auto& c : contexts_) if (!c.used) {
            c = Context{}; c.source = source; c.channel = channel; c.used = true;
            c.range = config_.bend_range;
            c.range_semitones = static_cast<uint8_t>(c.range > 127.f ? 127.f : c.range);
            c.range_cents = static_cast<uint8_t>(std::round((c.range - std::floor(c.range)) * 100.f));
            if (config_.enable_mpe) {
                const auto manager = mpe_.ManagerChannel(source, channel);
                if (manager != kAnyChannel && manager != channel)
                    if (const auto* owner = FindContext(source, manager)) {
                        c.sustain = owner->sustain; c.sostenuto = owner->sostenuto;
                        c.portamento = owner->portamento;
                    }
            }
            return &c;
        }
        return nullptr;
    }
    const Context* FindContext(SourceId source, uint8_t channel) const {
        for (const auto& c : contexts_) if (c.used && c.source == source && c.channel == channel) return &c;
        return nullptr;
    }
    Context* FindContext(SourceId source, uint8_t channel) {
        for (auto& c : contexts_) if (c.used && c.source == source && c.channel == channel) return &c;
        return nullptr;
    }
    std::size_t PhysicalCount() const {
        std::size_t count = 0; for (const auto& n : notes_) count += n.used && !n.silenced && n.key_down; return count;
    }
    std::size_t EligibleCount() const {
        std::size_t count = 0; for (const auto& n : notes_) count += Eligible(n); return count;
    }
    std::size_t GroupCount(bool physical = false) const {
        std::size_t count = 0;
        for (const auto& v : voices_) count += v.state == State::Held || (!physical && v.state == State::Pedal);
        return count;
    }
    NoteExpression Expression(const Note& n) const {
        if (config_.enable_mpe && mpe_.Role(n.token.source, n.token.channel) != MpeRole::None) {
            auto expression = mpe_.Expression(n.token.source, n.token.channel);
            if (n.member_frozen) {
                expression.bend_semitones = n.frozen.bend_semitones;
                expression.pressure = n.frozen.pressure;
                expression.timbre = n.frozen.timbre;
            }
            return expression;
        }
        NoteExpression expression;
        if (const auto* c = FindContext(n.token.source, n.token.channel)) {
            expression.bend_semitones = Bipolar14(c->bend) * c->range;
            expression.pressure = c->pressure / 127.f; expression.timbre = c->timbre / 127.f;
        }
        return expression;
    }
    NoteAction Action(uint16_t slot, ActionKind kind) const {
        const auto& v = voices_[slot];
        NoteAction a;
        a.kind = kind; a.voice = {slot, v.generation}; a.note = v.note;
        a.kill = kind == ActionKind::Kill;
        a.expression = v.expression; a.expression.key_pressure = v.key_pressure;
        a.pitch_semitones = v.note.key; a.velocity = v.velocity;
        a.release_velocity = v.release_velocity; a.note_off_origin = v.note_off_origin;
        a.gate = v.state == State::Held || v.state == State::Pedal;
        a.group_generation = group_generation_;
        return a;
    }
    static bool Attack(AttackPolicy policy, bool first_physical, bool first_eligible) {
        return policy == AttackPolicy::EachSelectedNote ||
            (policy == AttackPolicy::FirstPhysicalKey && first_physical) ||
            (policy == AttackPolicy::FirstEligibleNote && first_eligible);
    }
    PitchTransition Transition(const Note& n, bool overlap, bool eligible_overlap, bool returning) const {
        const auto& trajectory = config_.slide.transition;
        PitchTransition transition{false, trajectory.curve, trajectory.timing,
                                   trajectory.time_ms, trajectory.semitones_per_second};
        const auto* context = FindContext(n.token.source, n.token.channel);
        transition.slide = config_.slide.trigger == SlideTrigger::Always ||
            (config_.slide.trigger == SlideTrigger::KeyOverlap && overlap) ||
            (config_.slide.trigger == SlideTrigger::EligibleOverlap && eligible_overlap) ||
            (config_.slide.trigger == SlideTrigger::Controller && context && context->portamento);
        if (returning && !config_.slide.return_to_held) transition.slide = false;
        return transition;
    }
    template <class Sink>
    void Bind(uint16_t slot, uint16_t index, bool attack_amp, bool attack_filter,
              bool onset, PitchTransition transition, bool retarget, Sink& sink) {
        auto& v = voices_[slot];
        if (v.note_index < Notes && notes_[v.note_index].used && notes_[v.note_index].token == v.note)
            notes_[v.note_index].voice = kNoVoice;
        auto& n = notes_[index];
        v.generation = Next(v.generation); v.note = n.token; v.note_index = index;
        v.state = n.key_down ? State::Held : State::Pedal;
        v.order = n.order; v.release_order = 0; v.velocity = n.velocity;
        v.release_velocity = n.release_velocity; v.note_off_origin = n.note_off_origin;
        v.pitch_valid = true; v.level = 1.f; v.key_pressure = n.key_pressure;
        v.member_frozen = n.member_frozen; v.expression = Expression(n); v.group_tail = false;
        n.voice = slot;
        auto a = Action(slot, retarget ? ActionKind::Retarget : ActionKind::Assign);
        a.pitch_changed = true; a.attack_amp = attack_amp; a.attack_filter = attack_filter;
        a.onset = onset; a.pitch_transition = transition;
        sink(a);
    }
    uint16_t Selected() const {
        uint16_t result = kNoVoice;
        bool physical = false;
        for (uint16_t index = 0; index < Notes; ++index) {
            const auto& n = notes_[index];
            if (!Eligible(n)) continue;
            if (n.key_down && !physical) { physical = true; result = kNoVoice; }
            if (physical && !n.key_down) continue;
            if (result == kNoVoice) { result = index; continue; }
            const auto& current = notes_[result];
            const bool better = config_.priority == NotePriority::Last ? n.order > current.order
                : config_.priority == NotePriority::Low ? n.token.key < current.token.key
                : n.token.key > current.token.key;
            if (better || (n.token.key == current.token.key && n.order > current.order)) result = index;
        }
        if (!physical && selected_ < Notes && Eligible(notes_[selected_])) return selected_;
        return result;
    }
    template <class Sink>
    void SelectMono(bool note_on, bool first_physical, bool first_eligible, Sink& sink) {
        const auto next = Selected();
        if (next == kNoVoice) {
            if (voices_[0].state == State::Held || voices_[0].state == State::Pedal) ReleaseVoice(0, false, sink);
            selected_ = kNoVoice; return;
        }
        auto& v = voices_[0];
        if (next == selected_ && v.note == notes_[next].token) {
            v.state = notes_[next].key_down ? State::Held : State::Pedal;
            v.expression = Expression(notes_[next]);
            return;
        }
        const bool busy = v.state != State::Free;
        const bool amp = note_on ? Attack(config_.articulation.amp, first_physical, first_eligible)
            : !busy && Attack(config_.articulation.amp, true, true);
        const bool filter = note_on ? Attack(config_.articulation.filter, first_physical, first_eligible)
            : !busy && Attack(config_.articulation.filter, true, true);
        const bool onset = config_.articulation.onset == OnsetPolicy::EveryAttack ? amp || filter
            : config_.articulation.onset == OnsetPolicy::FirstPhrase && (note_on ? first_physical : !busy);
        auto transition = Transition(notes_[next], note_on ? !first_physical : busy,
                                     note_on ? !first_eligible : busy, !note_on);
        // DSP idle ends the envelope, not the mono instrument's pitch history.
        if (!v.pitch_valid) transition.slide = false;
        selected_ = next;
        Bind(0, next, amp, filter, onset, transition, busy, sink);
    }
    uint16_t Allocate(bool retiring_group = false) {
        for (std::size_t offset = 0; offset < Voices; ++offset) {
            const auto slot = static_cast<uint16_t>(config_.allocation == Allocation::LowestFree
                ? offset : (cursor_ + offset) % Voices);
            if (voices_[slot].state == State::Free ||
                (retiring_group && voices_[slot].state == State::Released && voices_[slot].group_tail)) {
                cursor_ = static_cast<uint16_t>((slot + 1) % Voices); return slot;
            }
        }
        if (config_.steal == StealPolicy::None) return kNoVoice;
        uint16_t best = 0;
        for (uint16_t slot = 1; slot < Voices; ++slot) {
            const auto& candidate = voices_[slot]; const auto& current = voices_[best];
            const auto rank = [this](const Voice& voice) {
                return config_.steal == StealPolicy::ReleasedThenSustainedThenHeld
                    ? voice.state == State::Released ? 0 : voice.state == State::Pedal ? 1 : 2 : 0;
            };
            bool better = rank(candidate) < rank(current);
            if (rank(candidate) == rank(current)) {
                if (config_.steal == StealPolicy::Quietest && candidate.level != current.level)
                    better = candidate.level < current.level;
                else {
                    const bool released_age = config_.steal == StealPolicy::ReleasedThenSustainedThenHeld;
                    better = (released_age && candidate.state == State::Released ? candidate.release_order : candidate.order)
                           < (released_age && current.state == State::Released ? current.release_order : current.order);
                }
            }
            if (better) best = slot;
        }
        cursor_ = static_cast<uint16_t>((best + 1) % Voices);
        return best;
    }
    template <class Sink>
    void NoteOn(const Event& e, Sink& sink) {
        const float velocity = Velocity(e);
        const bool first_physical = PhysicalCount() == 0, first_eligible = EligibleCount() == 0;
        uint16_t index = kNoVoice;
        if (config_.duplicates == DuplicatePolicy::RetriggerSingle)
            for (uint16_t i = 0; i < Notes; ++i) if (notes_[i].used && !notes_[i].silenced && notes_[i].key_down &&
                notes_[i].token.source == e.source && notes_[i].token.channel == e.channel && notes_[i].token.key == e.data1) {
                index = i; break;
            }
        if (index == kNoVoice) for (uint16_t i = 0; i < Notes; ++i) if (!notes_[i].used) { index = i; break; }
        if (index == kNoVoice) { Fail(e.source, sink); return; }
        const auto previous_voice = notes_[index].voice;
        notes_[index] = Note{};
        auto& n = notes_[index]; n.used = n.key_down = true; n.velocity = velocity;
        occurrence_ = Next(occurrence_); n.token = {e.source, occurrence_, e.channel, e.data1}; n.order = ++order_;
        if (config_.mode == NoteMode::Mono) { SelectMono(true, first_physical, first_eligible, sink); return; }
        const bool group_first_physical = GroupCount(true) == 0, group_first_eligible = GroupCount() == 0;
        auto transition = Transition(n, !first_physical, !first_eligible, false);
        const bool retiring_group = config_.mode == NoteMode::Paraphonic && !group_gate_ && group_busy_;
        const auto slot = previous_voice < Voices ? previous_voice : Allocate(retiring_group);
        if (slot == kNoVoice) return;
        if (voices_[slot].state == State::Free) transition.slide = false;
        if (retiring_group)
            for (uint16_t old = 0; old < Voices; ++old) if (voices_[old].group_tail)
                KillVoice(old, sink, old == slot && transition.slide);
        KillVoice(slot, sink, transition.slide);
        if (config_.mode == NoteMode::Poly) {
            const bool amp = Attack(config_.articulation.amp, true, true);
            const bool filter = Attack(config_.articulation.filter, true, true);
            const bool onset = config_.articulation.onset != OnsetPolicy::Never &&
                (config_.articulation.onset != OnsetPolicy::EveryAttack || amp || filter);
            Bind(slot, index, amp, filter, onset, transition, false, sink);
        } else {
            if (group_first_eligible) {
                group_generation_ = Next(group_generation_);
                group_first_velocity_ = group_max_velocity_ = n.velocity;
            }
            group_last_velocity_ = n.velocity;
            Bind(slot, index, false, false, false, transition, false, sink);
            UpdateGroupVelocity();
            const bool amp = Attack(config_.shared_amp_attack, group_first_physical, group_first_eligible);
            const bool filter = Attack(config_.shared_filter_attack, group_first_physical, group_first_eligible);
            group_gate_ = group_busy_ = true;
            EmitGroup(true, amp, filter, false, sink, group_first_physical);
        }
    }
    template <class Sink>
    void NoteOff(const Event& e, Sink& sink) {
        const float velocity = Velocity(e);
        uint16_t index = kNoVoice;
        for (uint16_t i = 0; i < Notes; ++i) {
            const auto& n = notes_[i];
            if (!n.used || !n.key_down || n.token.source != e.source || n.token.channel != e.channel || n.token.key != e.data1) continue;
            // CC120 does not replace NoteOff: consume its silent pending releases
            // before matching a subsequently played audible duplicate.
            if (index == kNoVoice || (n.silenced && !notes_[index].silenced) ||
                (n.silenced == notes_[index].silenced &&
                 (config_.duplicates == DuplicatePolicy::Lifo ? n.order > notes_[index].order
                                                             : n.order < notes_[index].order))) index = i;
        }
        if (index == kNoVoice) return;
        auto& n = notes_[index];
        n.frozen = Expression(n); n.member_frozen = config_.enable_mpe && mpe_.Role(e.source, e.channel) == MpeRole::Member;
        n.key_down = false; n.release_velocity = velocity;
        n.note_off_origin = e.type == EventType::NoteOn ? NoteOffOrigin::ZeroVelocityNoteOn : e.note_off_origin;
        const auto* c = FindContext(e.source, e.channel);
        n.sustain = config_.sustain && c && c->sustain;
        FinishRelease(index, sink);
    }
    template <class Sink>
    void FinishRelease(uint16_t index, Sink& sink) {
        auto& n = notes_[index];
        if (n.voice < Voices) {
            auto& v = voices_[n.voice];
            v.expression = Expression(n); v.member_frozen = n.member_frozen;
            v.release_velocity = n.release_velocity; v.note_off_origin = n.note_off_origin;
            if (Eligible(n)) v.state = n.key_down ? State::Held : State::Pedal;
        }
        if (config_.mode == NoteMode::Mono) SelectMono(false, false, false, sink);
        else if (!Eligible(n) && n.voice < Voices) {
            const bool group_tail = config_.mode == NoteMode::Paraphonic && GroupCount() == 1;
            ReleaseVoice(n.voice, group_tail, sink);
            if (config_.mode == NoteMode::Paraphonic) {
                UpdateGroupVelocity();
                if (!GroupCount() && group_gate_) {
                    group_gate_ = false; EmitGroup(false, false, false, false, sink);
                } else if (GroupCount()) EmitGroup(true, false, false, false, sink);
            }
        }
        if (!Eligible(n) && !n.key_down) { n.used = false; n.voice = kNoVoice; }
    }
    template <class Sink>
    void ReleaseVoice(uint16_t slot, bool group_tail, Sink& sink) {
        auto& v = voices_[slot];
        if (v.state == State::Free || v.state == State::Released) return;
        v.state = State::Released; v.release_order = ++order_; v.group_tail = group_tail;
        auto a = Action(slot, ActionKind::Release); a.retain_for_group_release = group_tail;
        sink(a);
    }
    template <class Sink>
    void KillVoice(uint16_t slot, Sink& sink, bool retain_pitch = false) {
        auto& v = voices_[slot];
        if (v.state == State::Free) { v.pitch_valid = false; return; }
        if (v.note_index < Notes && notes_[v.note_index].used && notes_[v.note_index].token == v.note)
            notes_[v.note_index].voice = kNoVoice;
        auto a = Action(slot, ActionKind::Kill); a.gate = false; a.retain_pitch = retain_pitch; sink(a);
        v.state = State::Free; v.note_index = kNoVoice; v.group_tail = false; v.pitch_valid = retain_pitch;
    }
    template <class Sink>
    void EmitGroup(bool gate, bool amp, bool filter, bool kill, Sink& sink, bool first_phrase = false) const {
        NoteAction a; a.kind = ActionKind::Group; a.gate = gate; a.attack_amp = amp; a.attack_filter = filter;
        a.velocity = group_velocity_; a.group_generation = group_generation_;
        a.onset = !kill && (config_.articulation.onset == OnsetPolicy::FirstPhrase ? first_phrase
            : config_.articulation.onset == OnsetPolicy::EveryAttack && (amp || filter));
        a.retain_for_group_release = !gate && !kill;
        a.kill = kill;
        sink(a);
    }
    void UpdateGroupVelocity() {
        if (GroupCount()) {
            group_max_velocity_ = 0.f;
            for (const auto& v : voices_) if (v.state == State::Held || v.state == State::Pedal)
                if (v.velocity > group_max_velocity_) group_max_velocity_ = v.velocity;
        }
        group_velocity_ = config_.shared_velocity == SharedVelocity::Fixed ? config_.fixed_velocity
            : config_.shared_velocity == SharedVelocity::PhraseFirst ? group_first_velocity_
            : config_.shared_velocity == SharedVelocity::LastAttack ? group_last_velocity_ : group_max_velocity_;
    }
    uint16_t Scope(const Event& e) const {
        return config_.enable_mpe && mpe_.Role(e.source, e.channel) != MpeRole::None
            ? mpe_.Scope(e.source, e.channel) : Bit(e.channel);
    }
    float Velocity(const Event& e) {
        auto* context = GetContext(e.source, e.channel);
        const bool prefix = context && context->velocity_prefix && config_.high_resolution_velocity;
        const uint16_t fine = context ? context->velocity_lsb : 0;
        if (context) context->velocity_prefix = false;
        if (!e.value && (e.type == EventType::NoteOn || e.note_off_origin != NoteOffOrigin::Explicit))
            return 0.f;
        return e.resolution == 14 ? e.value / 16383.f
             : prefix ? ((e.value << 7) | fine) / 16383.f : e.value / 127.f;
    }
    template <class Sink>
    void EmitExpression(Voice& v, Sink& sink) {
        if (v.note_index < Notes && notes_[v.note_index].used && notes_[v.note_index].token == v.note)
            v.expression = Expression(notes_[v.note_index]);
        else {
            Note n; n.token = v.note; n.frozen = v.expression; n.member_frozen = v.member_frozen;
            v.expression = Expression(n);
        }
        const auto slot = static_cast<uint16_t>(&v - voices_.data());
        sink(Action(slot, ActionKind::Expression));
    }
    template <class Sink>
    void UpdateExpression(SourceId source, uint16_t mask, Sink& sink) {
        for (auto& v : voices_) if (v.state != State::Free && v.note.source == source &&
            (mask & Bit(v.note.channel))) EmitExpression(v, sink);
    }
    template <class Sink>
    void SetPedal(SourceId source, uint16_t mask, bool sostenuto, bool down, Sink& sink) {
        for (auto& c : contexts_) if (c.used && c.source == source && (mask & Bit(c.channel))) {
            if (sostenuto) c.sostenuto = down; else c.sustain = down;
        }
        if (down && sostenuto) {
            for (auto& n : notes_) if (n.used && !n.silenced && n.key_down && n.token.source == source && (mask & Bit(n.token.channel)))
                n.sostenuto = true;
        }
        if (down && !sostenuto) {
            for (auto& n : notes_) if (Eligible(n) && !n.key_down && n.token.source == source &&
                (mask & Bit(n.token.channel))) n.sustain = true;
        }
        if (!down) for (uint16_t i = 0; i < Notes; ++i) {
            auto& n = notes_[i];
            if (!n.used || n.token.source != source || !(mask & Bit(n.token.channel))) continue;
            if (sostenuto) n.sostenuto = false; else n.sustain = false;
            if (!n.key_down && config_.mode != NoteMode::Mono) FinishRelease(i, sink);
        }
        if (!down && config_.mode==NoteMode::Mono) FinishMonoBatch(sink);
    }
    template <class Sink>
    void FinishMonoBatch(Sink& sink) {
        for (auto& n : notes_) {
            if (!n.used) continue;
            if (n.voice < Voices && voices_[n.voice].note == n.token) {
                auto& v=voices_[n.voice];
                v.expression=Expression(n); v.member_frozen=n.member_frozen;
                v.release_velocity=n.release_velocity; v.note_off_origin=n.note_off_origin;
            }
            if (!Eligible(n) && !n.key_down) {n.used=false; n.voice=kNoVoice;}
        }
        SelectMono(false,false,false,sink);
    }
    template <class Sink>
    void AllNotesOff(SourceId source, uint16_t mask, Sink& sink) {
        for (auto& n : notes_) if (n.used && n.key_down && n.token.source == source && (mask & Bit(n.token.channel))) {
            n.frozen = Expression(n); n.member_frozen = config_.enable_mpe && mpe_.Role(source, n.token.channel) == MpeRole::Member;
            n.key_down = false; n.note_off_origin = NoteOffOrigin::Unknown;
            const auto* c = FindContext(source, n.token.channel);
            n.sustain = config_.sustain && c && c->sustain;
        }
        if (config_.mode==NoteMode::Mono) FinishMonoBatch(sink);
        else for (uint16_t i = 0; i < Notes; ++i) if (notes_[i].used && notes_[i].token.source == source &&
            (mask & Bit(notes_[i].token.channel))) FinishRelease(i, sink);
    }
    template <class Sink>
    void Control(const Event& e, Context& c, MpeRole role, Sink& sink) {
        const auto scope = Scope(e);
        switch (e.data1) {
        case 64:
            if (config_.sustain && c.sustain != (e.value >= 64)) SetPedal(e.source, scope, false, e.value >= 64, sink);
            break;
        case 66:
            if (config_.sostenuto && c.sostenuto != (e.value >= 64)) SetPedal(e.source, scope, true, e.value >= 64, sink);
            break;
        case 65:
            for (auto& context : contexts_) if (context.used && context.source == e.source &&
                (scope & Bit(context.channel))) context.portamento = e.value >= 64;
            break;
        case 74: c.timbre = static_cast<uint8_t>(e.value); UpdateExpression(e.source, scope, sink); break;
        case 88:
            if (config_.high_resolution_velocity) { c.velocity_lsb = static_cast<uint8_t>(e.value); c.velocity_prefix = true; }
            break;
        case 120: SilenceScope(e.source, scope, sink); break;
        case 121:
            for (auto& n : notes_) if (n.used && n.token.source == e.source && (scope & Bit(n.token.channel))) {
                n.key_pressure = 0.f;
                n.sustain = n.sostenuto = false;
            }
            for (auto& v : voices_) if (v.state != State::Free && v.note.source == e.source &&
                (scope & Bit(v.note.channel))) v.key_pressure = 0.f;
            for (auto& context : contexts_) if (context.used && context.source == e.source && (scope & Bit(context.channel))) {
                context.bend = 8192; context.pressure = 0; context.portamento = false;
                context.sustain = context.sostenuto = false;
                context.velocity_prefix = false;
                context.rpn_msb = context.rpn_lsb = 127; context.rpn = true;
            }
            if (config_.mode == NoteMode::Mono) FinishMonoBatch(sink);
            else for (uint16_t i = 0; i < Notes; ++i) {
                const auto& note = notes_[i];
                if (note.used && !note.key_down && note.token.source == e.source &&
                    (scope & Bit(note.token.channel))) FinishRelease(i, sink);
            }
            UpdateExpression(e.source, scope, sink);
            break;
        case 123: AllNotesOff(e.source, scope, sink); break;
        case 124: case 125:
            if (role != MpeRole::None) break;
            AllNotesOff(e.source, Bit(e.channel), sink);
            break;
        case 126: case 127:
            if (role != MpeRole::None) {
                const auto manager = mpe_.ManagerChannel(e.source, e.channel);
                const auto members = mpe_.Scope(e.source, manager) & ~Bit(manager);
                uint8_t basic = 0;
                while (basic < 16 && !(members & Bit(basic))) ++basic;
                if (e.channel != basic) break;
            }
            AllNotesOff(e.source, Bit(e.channel), sink);
            break;
        case 101: c.rpn_msb = static_cast<uint8_t>(e.value); c.rpn = true; break;
        case 100: c.rpn_lsb = static_cast<uint8_t>(e.value); c.rpn = true; break;
        case 99: case 98: c.rpn = false; break;
        case 6: case 38:
            if (c.rpn && c.rpn_msb == 0 && c.rpn_lsb == 0 && role == MpeRole::None) {
                if (e.data1 == 6) { c.range_semitones = static_cast<uint8_t>(e.value); c.range_cents = 0; }
                else c.range_cents = static_cast<uint8_t>(e.value);
                c.range = c.range_semitones + c.range_cents / 100.f;
                UpdateExpression(e.source, scope, sink);
            }
            break;
        default: break;
        }
    }
    template <class Sink>
    void ResetScope(SourceId source, uint16_t mask, Sink& sink) {
        for (auto& n : notes_) if (n.used && n.token.source == source && (mask & Bit(n.token.channel))) n = Note{};
        for (auto& c : contexts_) if (c.used && c.source == source && (mask & Bit(c.channel))) c = Context{};
        const bool mono_return = config_.mode == NoteMode::Mono && Selected() != kNoVoice;
        bool changed_voice = false;
        for (uint16_t slot = 0; slot < Voices; ++slot) if (voices_[slot].note.source == source &&
            (mask & Bit(voices_[slot].note.channel))) {
            if (voices_[slot].state == State::Free) { voices_[slot].pitch_valid = false; continue; }
            changed_voice = true;
            if (!mono_return) KillVoice(slot, sink);
        }
        if (config_.mode == NoteMode::Mono) SelectMono(false, false, false, sink);
        UpdateGroupAfterKill(changed_voice, sink);
    }
    template <class Sink>
    void SilenceScope(SourceId source, uint16_t mask, Sink& sink) {
        for (auto& n : notes_) if (n.used && n.token.source == source && (mask & Bit(n.token.channel))) {
            if (!n.key_down) n = Note{};
            else { n.silenced = true; n.sustain = n.sostenuto = false; }
        }
        bool changed_voice = false;
        for (uint16_t slot = 0; slot < Voices; ++slot) if (voices_[slot].note.source == source &&
            (mask & Bit(voices_[slot].note.channel))) {
            changed_voice |= voices_[slot].state != State::Free;
            KillVoice(slot, sink);
        }
        if (config_.mode == NoteMode::Mono) SelectMono(false, false, false, sink);
        UpdateGroupAfterKill(changed_voice, sink);
    }
    template <class Sink>
    void UpdateGroupAfterKill(bool changed_voice, Sink& sink) {
        if (config_.mode == NoteMode::Paraphonic && changed_voice && group_busy_) {
            if (GroupCount()) {
                group_gate_ = true; UpdateGroupVelocity();
                EmitGroup(true, false, false, false, sink);
            } else {
                bool retained_tail = false;
                for (const auto& v : voices_)
                    retained_tail |= v.state == State::Released && v.group_tail;
                if (!retained_tail) {
                    EmitGroup(false, false, false, true, sink); group_gate_ = group_busy_ = false;
                }
            }
        }
    }
    template <class Sink>
    void Fail(SourceId source, Sink& sink) {
        if (failures_ != std::numeric_limits<uint32_t>::max()) ++failures_;
        ResetScope(source, 0xffffu, sink); mpe_.Reset(source);
        for (auto& state : sources_) if (state.used && state.source == source) {
            state.faulted = true; return;
        }
    }

    PartConfig config_{};
    std::array<Note, Notes> notes_{};
    std::array<Voice, Voices> voices_{};
    std::array<Context, Channels> contexts_{};
    std::array<Source, Channels> sources_{};
    MpeState<MpeSources> mpe_{};
    uint64_t order_ = 0;
    uint32_t occurrence_ = 0, failures_ = 0, group_generation_ = 0;
    uint16_t selected_ = kNoVoice, cursor_ = 0;
    bool group_gate_ = false, group_busy_ = false;
    float group_velocity_ = 1.f;
    float group_first_velocity_ = 1.f, group_last_velocity_ = 1.f, group_max_velocity_ = 1.f;
};

} // namespace alchemy::midi
