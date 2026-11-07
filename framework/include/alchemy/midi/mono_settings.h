#pragma once
#include "alchemy/midi/mono_options.h"
#include "alchemy/surface/settings.h"

namespace alchemy::midi {
class MonoSettings {
  public:
    bool Attach(alchemy::Settings& settings, uint8_t first_page) {
        if (first_page+1u>=kSettingsMaxPages) return false;
        settings.Page(first_page).Name("MIDI Notes");
        channel = settings.Page(first_page).Pot(0).Selector(kChannels).Default(1).Ident("midi.channel").Name("MIDI Channel");
        priority = settings.Page(first_page).Pot(1).Selector(kPriorities).Ident("midi.priority").Name("Note Priority");
        amp_attack = settings.Page(first_page).Pot(2).Selector(kAttack).Ident("midi.amp.attack").Name("VCA Retrigger");
        filter_attack = settings.Page(first_page).Pot(3).Selector(kAttack).Ident("midi.filter.attack").Name("Filter Retrigger");
        slide = settings.Page(first_page).Pot(4).Selector(kSlides).Ident("midi.slide").Name("Slide Mode");
        slide_time = settings.Page(first_page).Pot(5).Knob().Default(.12f).Ident("midi.slide.time").Name("Slide Time").Help("0–500 ms, exponential pitch trajectory; zero changes pitch immediately.");
        const uint8_t control_page=first_page+1;
        settings.Page(control_page).Name("MIDI Control");
        pitch_gate = settings.Page(control_page).Pot(0).Selector(kMerge).Default(2).Ident("midi.pitch.gate").Name("Pitch / Gate Source").Help("MIDI + CV adds V/oct to MIDI pitch and combines gates. Last Active follows the latest note or CV gate edge.");
        bend = settings.Page(control_page).Pot(1).Selector(kBends).Default(2).Ident("midi.bend").Name("Pitch Bend Range");
        velocity_level = settings.Page(control_page).Pot(2).Knob().Default(0).Ident("midi.velocity.level").Name("Velocity to Level");
        velocity_accent = settings.Page(control_page).Pot(3).Knob().Default(0).Ident("midi.velocity.accent").Name("Velocity to Accent");
        clock = settings.Page(control_page).Pot(4).Selector(kClock).Ident("midi.clock").Name("Clock Source");
        cc_merge = settings.Page(control_page).Pot(5).Selector(kCcMerge).Ident("midi.cc.merge").Name("CC / Panel Merge").Help("Add uses CC 64 as zero and adds to the panel/lock base. Replace overlays the panel base. CV and parameter locks remain additive.");
        return true;
    }
    MonoOptions Read() const {
        return {channel.Value(),priority.Value(),amp_attack.Value(),filter_attack.Value(),slide.Value(),
            pitch_gate.Value(),bend.Value(),clock.Value(),cc_merge.Value(),slide_time.Value(),
            velocity_level.Value(),velocity_accent.Value()};
    }
    SelectorHandle channel,priority,amp_attack,filter_attack,slide,pitch_gate,bend,clock,cc_merge;
    KnobHandle slide_time,velocity_level,velocity_accent;
    inline static constexpr const char* kChannels[] = {"Off","Omni","1","2","3","4","5","6","7","8","9","10","11","12","13","14","15","16"};
    inline static constexpr const char* kPriorities[] = {"Last","Low","High"};
    inline static constexpr const char* kAttack[] = {"Every Note","First Key"};
    inline static constexpr const char* kSlides[] = {"Off","Key Overlap","Held / Sustain Overlap","Always","CC65"};
    inline static constexpr const char* kMerge[] = {"CV","MIDI","MIDI + CV","Last Active"};
    inline static constexpr const char* kBends[] = {"0 semitones","1 semitone","2 semitones","3 semitones","5 semitones","7 semitones","12 semitones","24 semitones"};
    inline static constexpr const char* kClock[] = {"External Pulses","MIDI Transport","MIDI Tempo"};
    inline static constexpr const char* kCcMerge[] = {"Add","Replace"};
};
}
