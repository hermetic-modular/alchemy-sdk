/**
 * @file alchemy/midi/mono_note_tracker.h
 * @brief Held-note stack → one (gate, note, velocity) for mono voices.
 *        After the last release, Note()/Velocity() hold their final
 *        values so release tails keep pitch.
 */

#pragma once

#include <cstdint>

namespace alchemy {
namespace midi {

class MonoNoteTracker
{
  public:
    static constexpr uint8_t kCapacity = 16u;

    enum class Priority : uint8_t
    {
        Last = 0,
        Low  = 1,
        High = 2,
    };

    MonoNoteTracker& SetPriority(Priority p)
    {
        prio_ = p;
        return *this;
    }

    /** Retrigger when a release re-selects an older held note
     *  (default off: releasing back to a held note is legato). */
    MonoNoteTracker& RetrigOnReturn(bool on)
    {
        retrig_on_return_ = on;
        return *this;
    }

    void NoteOn(uint8_t note, uint8_t velocity);
    void NoteOff(uint8_t note);
    void SetSustain(bool on);
    void Clear();

    bool SustainActive() const { return sustain_; }

    bool    GateHigh() const { return n_ > 0u; }
    uint8_t Note() const { return cur_note_; }
    uint8_t Velocity() const { return cur_vel_; }
    uint8_t HeldCount() const { return n_; }

    /** One-shot: true once per (re)trigger edge. */
    bool TakeRetrigger()
    {
        const bool r = retrig_;
        retrig_      = false;
        return r;
    }

  private:
    struct Held
    {
        uint8_t note;
        uint8_t vel;
        bool    sustained;
    };

    uint8_t SelectIndex() const;
    void    Remove(uint8_t idx);
    void    Reselect();

    Held     held_[kCapacity] = {};
    uint8_t  n_               = 0u;
    Priority prio_            = Priority::Last;
    bool     retrig_on_return_ = false;
    bool     retrig_           = false;
    bool     sustain_          = false;
    uint8_t  cur_note_         = 60u;
    uint8_t  cur_vel_          = 0u;
};

} // namespace midi
} // namespace alchemy
