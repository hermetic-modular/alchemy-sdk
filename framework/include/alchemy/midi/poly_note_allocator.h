/**
 * @file alchemy/midi/poly_note_allocator.h
 * @brief N-voice note allocation for polyphonic firmware: voice
 *        stealing, sustain, per-voice retrigger.  Notes are keyed
 *        (channel, note), so an omni feed from member channels (MPE)
 *        allocates correctly; per-voice expression is read from the
 *        UsbMidi per-channel caches via Voice::channel.
 */

#pragma once

#include <cstdint>

namespace alchemy {
namespace midi {

class PolyNoteAllocator
{
  public:
    static constexpr uint8_t kMaxVoices = 16u;

    enum class Steal : uint8_t
    {
        None    = 0, /* drop new notes when full     */
        Oldest  = 1,
        Lowest  = 2,
        Highest = 3,
    };

    struct Voice
    {
        uint8_t note     = 60u;
        uint8_t velocity = 0u;
        uint8_t channel  = 0u;
        bool    gate     = false;
    };

    PolyNoteAllocator& SetVoiceCount(uint8_t n);
    PolyNoteAllocator& SetSteal(Steal s)
    {
        steal_ = s;
        return *this;
    }

    void NoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
    void NoteOff(uint8_t channel, uint8_t note);
    void SetSustain(bool on);
    void Clear();

    uint8_t      VoiceCount() const { return num_voices_; }
    const Voice& VoiceAt(uint8_t i) const { return voices_[i]; }
    bool         SustainActive() const { return sustain_; }
    uint8_t      GatedCount() const;

    /** One-shot per voice: true once per (re)trigger. */
    bool TakeRetrigger(uint8_t i)
    {
        const bool r = retrig_[i];
        retrig_[i]   = false;
        return r;
    }

  private:
    uint8_t FindGated(uint8_t channel, uint8_t note) const;
    uint8_t PickVoice();

    Voice    voices_[kMaxVoices];
    bool     retrig_[kMaxVoices]    = {};
    bool     sustained_[kMaxVoices] = {};
    uint32_t on_order_[kMaxVoices]  = {};
    uint32_t off_order_[kMaxVoices] = {};
    uint32_t counter_               = 0u;
    uint8_t  num_voices_            = kMaxVoices;
    Steal    steal_                 = Steal::Oldest;
    bool     sustain_               = false;
};

} // namespace midi
} // namespace alchemy
