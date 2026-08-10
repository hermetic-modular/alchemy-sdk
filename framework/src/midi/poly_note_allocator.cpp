#include "alchemy/midi/poly_note_allocator.h"

namespace alchemy {
namespace midi {

PolyNoteAllocator& PolyNoteAllocator::SetVoiceCount(uint8_t n)
{
    if (n < 1u) n = 1u;
    if (n > kMaxVoices) n = kMaxVoices;
    num_voices_ = n;
    Clear();
    return *this;
}

uint8_t PolyNoteAllocator::GatedCount() const
{
    uint8_t n = 0u;
    for (uint8_t i = 0u; i < num_voices_; i++)
        if (voices_[i].gate) n++;
    return n;
}

uint8_t PolyNoteAllocator::FindGated(uint8_t channel, uint8_t note) const
{
    for (uint8_t i = 0u; i < num_voices_; i++)
    {
        if (voices_[i].gate && voices_[i].note == note
            && voices_[i].channel == channel)
            return i;
    }
    return kMaxVoices;
}

uint8_t PolyNoteAllocator::PickVoice()
{
    /* Free voice, least-recently released first. */
    uint8_t  best      = kMaxVoices;
    uint32_t best_off  = 0u;
    bool     have_free = false;
    for (uint8_t i = 0u; i < num_voices_; i++)
    {
        if (!voices_[i].gate)
        {
            if (!have_free || off_order_[i] < best_off)
            {
                have_free = true;
                best      = i;
                best_off  = off_order_[i];
            }
        }
    }
    if (have_free) return best;

    switch (steal_)
    {
        case Steal::None: return kMaxVoices;
        case Steal::Oldest:
        {
            uint32_t oldest = 0u;
            for (uint8_t i = 0u; i < num_voices_; i++)
            {
                if (best == kMaxVoices || on_order_[i] < oldest)
                {
                    best   = i;
                    oldest = on_order_[i];
                }
            }
            return best;
        }
        case Steal::Lowest:
        case Steal::Highest:
        {
            for (uint8_t i = 0u; i < num_voices_; i++)
            {
                if (best == kMaxVoices) { best = i; continue; }
                const bool wins = (steal_ == Steal::Lowest)
                                      ? (voices_[i].note < voices_[best].note)
                                      : (voices_[i].note > voices_[best].note);
                if (wins) best = i;
            }
            return best;
        }
    }
    return kMaxVoices;
}

void PolyNoteAllocator::NoteOn(uint8_t channel, uint8_t note, uint8_t velocity)
{
    uint8_t v = FindGated(channel, note);
    if (v == kMaxVoices) v = PickVoice();
    if (v == kMaxVoices) return;

    voices_[v].note     = note;
    voices_[v].velocity = velocity;
    voices_[v].channel  = channel;
    voices_[v].gate     = true;
    sustained_[v]       = false;
    retrig_[v]          = true;
    on_order_[v]        = ++counter_;
}

void PolyNoteAllocator::NoteOff(uint8_t channel, uint8_t note)
{
    const uint8_t v = FindGated(channel, note);
    if (v == kMaxVoices) return;

    if (sustain_)
    {
        sustained_[v] = true;
        return;
    }
    voices_[v].gate = false;
    off_order_[v]   = ++counter_;
}

void PolyNoteAllocator::SetSustain(bool on)
{
    sustain_ = on;
    if (on) return;
    for (uint8_t i = 0u; i < num_voices_; i++)
    {
        if (sustained_[i])
        {
            sustained_[i]   = false;
            voices_[i].gate = false;
            off_order_[i]   = ++counter_;
        }
    }
}

void PolyNoteAllocator::Clear()
{
    for (uint8_t i = 0u; i < kMaxVoices; i++)
    {
        voices_[i].gate = false;
        retrig_[i]      = false;
        sustained_[i]   = false;
    }
    sustain_ = false;
}

} // namespace midi
} // namespace alchemy
