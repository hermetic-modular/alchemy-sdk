#pragma once
#include "alchemy/midi/note_part.h"

namespace alchemy::midi {
enum class ClockInput : uint8_t { External, MidiTransport, MidiTempo };
enum class CcMerge : uint8_t { Add, Replace };
struct MonoOptions {
    // Settings index: Off, Omni, then channels 1..16.
    uint8_t channel = 1, priority = 0, amp_attack = 0, filter_attack = 0, slide = 0;
    uint8_t pitch_gate = 2, bend = 2, clock = 0, cc_merge = 0;
    float slide_time = 0.12f, velocity_level = 0.f, velocity_accent = 0.f;
    void Disable() { channel = 0; }
    void SetOmni() { channel = 1; }
    bool SetChannel(uint8_t one_based) {
        if (one_based < 1 || one_based > 16) return false;
        channel = one_based + 1;
        return true;
    }
    bool Omni() const { return channel == 1; }
    uint8_t Channel() const { return channel >= 2 && channel < 18 ? channel - 1 : 0; }
    bool Enabled() const { return channel > 0 && channel < 18; }
    Listen Listening() const {
        if (!Enabled()) return {0, kAnyPort};
        return Omni() ? Listen::Omni() : Listen::Channel(Channel());
    }
    PartConfig Part() const {
        PartConfig result;
        if (!Valid()) { result.listen.channel_mask=0; return result; }
        result.listen = Listening();
        if (!Enabled()) result.mode = NoteMode::Off;
        result.priority = static_cast<NotePriority>(priority);
        result.articulation.amp = amp_attack ? AttackPolicy::FirstPhysicalKey : AttackPolicy::EachSelectedNote;
        result.articulation.filter = filter_attack ? AttackPolicy::FirstPhysicalKey : AttackPolicy::EachSelectedNote;
        constexpr SlideTrigger triggers[] = {SlideTrigger::Off, SlideTrigger::KeyOverlap,
            SlideTrigger::EligibleOverlap, SlideTrigger::Always, SlideTrigger::Controller};
        result.slide.trigger = triggers[slide];
        result.slide.transition.time_ms = slide_time * 500.f;
        constexpr float bends[] = {0,1,2,3,5,7,12,24};
        result.bend_range = bends[bend];
        return result;
    }
    PitchGateConfig Merge() const {
        constexpr PitchMerge pitches[] = {PitchMerge::Cv, PitchMerge::Midi, PitchMerge::MidiPlusCv, PitchMerge::LastActive};
        constexpr GateMerge gates[] = {GateMerge::Cv, GateMerge::Midi, GateMerge::Union, GateMerge::Union};
        PitchGateConfig result;
        const bool enabled = Valid() && Enabled();
        result.pitch = enabled ? pitches[pitch_gate] : PitchMerge::Cv;
        result.gate = enabled ? gates[pitch_gate] : GateMerge::Cv;
        result.cv_attacks = !enabled || pitch_gate != 1;
        result.midi_attacks = enabled && pitch_gate != 0;
        return result;
    }
    bool operator==(const MonoOptions& b) const {
        return channel==b.channel && priority==b.priority && amp_attack==b.amp_attack &&
            filter_attack==b.filter_attack && slide==b.slide && pitch_gate==b.pitch_gate &&
            bend==b.bend && clock==b.clock && cc_merge==b.cc_merge && slide_time==b.slide_time &&
            velocity_level==b.velocity_level && velocity_accent==b.velocity_accent;
    }
    bool Valid() const {
        return channel<18 && priority<3 && amp_attack<2 && filter_attack<2 && slide<5 &&
            pitch_gate<4 && bend<8 && clock<3 && cc_merge<2 && Unit(slide_time) &&
            Unit(velocity_level) && Unit(velocity_accent);
    }
  private:
    static bool Unit(float v) { return std::isfinite(v) && v>=0.f && v<=1.f; }
};

}
