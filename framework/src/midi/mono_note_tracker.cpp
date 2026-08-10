#include "alchemy/midi/mono_note_tracker.h"

namespace alchemy {
namespace midi {

uint8_t MonoNoteTracker::SelectIndex() const
{
    if (prio_ == Priority::Last)
    {
        return static_cast<uint8_t>(n_ - 1u);
    }

    uint8_t best = 0u;
    for (uint8_t i = 1u; i < n_; i++)
    {
        const bool wins = (prio_ == Priority::Low)
                              ? (held_[i].note < held_[best].note)
                              : (held_[i].note > held_[best].note);
        if (wins)
        {
            best = i;
        }
    }
    return best;
}

void MonoNoteTracker::Remove(uint8_t idx)
{
    for (uint8_t i = idx; i + 1u < n_; i++)
    {
        held_[i] = held_[i + 1u];
    }
    n_--;
}

void MonoNoteTracker::Reselect()
{
    if (n_ == 0u)
    {
        return;
    }
    const uint8_t sel = SelectIndex();
    if (held_[sel].note != cur_note_)
    {
        cur_note_ = held_[sel].note;
        cur_vel_  = held_[sel].vel;
        if (retrig_on_return_)
        {
            retrig_ = true;
        }
    }
}

void MonoNoteTracker::NoteOn(uint8_t note, uint8_t velocity)
{
    for (uint8_t i = 0u; i < n_; i++)
    {
        if (held_[i].note == note)
        {
            Remove(i);
            break;
        }
    }
    if (n_ == kCapacity)
    {
        Remove(0u);
    }

    const bool was_gated = (n_ > 0u);
    held_[n_++]          = Held{note, velocity, false};

    const uint8_t sel = SelectIndex();
    if (!was_gated || held_[sel].note != cur_note_
        || held_[sel].note == note)
    {
        if (held_[sel].note == note || !was_gated)
        {
            retrig_ = true;
        }
        cur_note_ = held_[sel].note;
        cur_vel_  = held_[sel].vel;
    }
}

void MonoNoteTracker::NoteOff(uint8_t note)
{
    for (uint8_t i = 0u; i < n_; i++)
    {
        if (held_[i].note == note)
        {
            if (sustain_)
            {
                held_[i].sustained = true;
                return;
            }
            Remove(i);
            Reselect();
            return;
        }
    }
}

void MonoNoteTracker::SetSustain(bool on)
{
    sustain_ = on;
    if (on)
    {
        return;
    }
    for (uint8_t i = n_; i > 0u; i--)
    {
        if (held_[i - 1u].sustained)
        {
            Remove(static_cast<uint8_t>(i - 1u));
        }
    }
    Reselect();
}

void MonoNoteTracker::Clear()
{
    n_       = 0u;
    retrig_  = false;
    sustain_ = false;
}

} // namespace midi
} // namespace alchemy
