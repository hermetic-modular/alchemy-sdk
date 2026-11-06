#include "alchemy/control/musical_clock.h"

#include <cassert>
#include <cmath>

namespace alchemy {

namespace {

// A zero period marks a subdivision this PPQN cannot represent.
inline uint32_t Fire(uint64_t first_tick, uint64_t last_tick, uint32_t period, uint32_t flag)
{
    if (period == 0u) return 0u;
    const auto phase = first_tick % period;
    return (phase == 0u || last_tick - first_tick >= period - phase) ? flag : 0u;
}

} // namespace

MusicalClock::MusicalClock(uint32_t ppqn, uint8_t beats_per_bar)
{
    assert(ppqn > 0u && (ppqn % 24u) == 0u);
    assert(beats_per_bar > 0u);

    ppqn_          = ppqn;
    beats_per_bar_ = beats_per_bar;
    RecomputePeriods();
}

void MusicalClock::RecomputePeriods()
{
    /* Straight: power-of-two divisions of the quarter.  ppqn=24 cannot
     * represent 64ths exactly; signal that with 0. */
    p_64th_     = (ppqn_ % 16u == 0u) ? ppqn_ / 16u : 0u;
    p_32nd_     = (ppqn_ %  8u == 0u) ? ppqn_ /  8u : 0u;
    p_16th_     = (ppqn_ %  4u == 0u) ? ppqn_ /  4u : 0u;
    p_8th_      = (ppqn_ %  2u == 0u) ? ppqn_ /  2u : 0u;
    p_quarter_  = ppqn_;
    p_half_     = ppqn_ * 2u;
    p_whole_    = ppqn_ * 4u;
    p_bar_      = ppqn_ * static_cast<uint32_t>(beats_per_bar_);
    p_2bar_     = p_bar_ * 2u;
    p_4bar_     = p_bar_ * 4u;
    p_8bar_     = p_bar_ * 8u;

    /* Triplets: three notes per straight subdivision.  ppqn must be
     * divisible by 3 for triplets to be exact, which any multiple of 24
     * satisfies. */
    p_32nd_t_    = (ppqn_ % 12u == 0u) ? ppqn_ / 12u : 0u;
    p_16th_t_    = (ppqn_ %  6u == 0u) ? ppqn_ /  6u : 0u;
    p_8th_t_     = (ppqn_ %  3u == 0u) ? ppqn_ /  3u : 0u;
    p_quarter_t_ = ((2u * ppqn_) % 3u == 0u) ? (2u * ppqn_) / 3u : 0u;
    p_half_t_    = ((4u * ppqn_) % 3u == 0u) ? (4u * ppqn_) / 3u : 0u;
    p_whole_t_   = ((8u * ppqn_) % 3u == 0u) ? (8u * ppqn_) / 3u : 0u;

    /* Dotted: 1.5× the straight subdivision. */
    p_dotted_16th_  = ((3u * ppqn_) % 8u == 0u) ? (3u * ppqn_) / 8u : 0u;
    p_dotted_8th_   = ((3u * ppqn_) % 4u == 0u) ? (3u * ppqn_) / 4u : 0u;
    p_dotted_qtr_   = ((3u * ppqn_) % 2u == 0u) ? (3u * ppqn_) / 2u : 0u;
    p_dotted_half_  = 3u * ppqn_;
    p_dotted_whole_ = 6u * ppqn_;

    /* 24 PPQN MIDI-clock grid.  Guaranteed integer by the ppqn%24 invariant. */
    p_midi_clock_ = ppqn_ / kClockExtPpqnDefault;
}

void MusicalClock::Start() { pending_start_ = true; pending_stop_ = false; }
void MusicalClock::Stop()  { pending_stop_ = true; pending_start_ = false; }
void MusicalClock::Reset() { pending_reset_ = true; pending_seek_ = false; }

void MusicalClock::Seek(uint64_t master_tick)
{
    pending_seek_tick_ = master_tick;
    pending_seek_ = true;
    pending_reset_ = false;
}

void MusicalClock::SetBpm(float bpm)
{
    if (steered_ || !std::isfinite(bpm)) return;
    if (bpm < kClockBpmMin) bpm = kClockBpmMin;
    if (bpm > kClockBpmMax) bpm = kClockBpmMax;
    ticks_per_us_ = (static_cast<double>(bpm) * static_cast<double>(ppqn_))
                  / (60.0 * 1e6);
}

void MusicalClock::SetTimeSignature(uint8_t beats_per_bar)
{
    assert(beats_per_bar > 0u);
    beats_per_bar_ = beats_per_bar;
    RecomputePeriods();
    SetPosition(master_tick_);
}

void MusicalClock::SetTicksPerUs(double tpu)
{
    if (!std::isfinite(tpu)) return;
    if (tpu < 0.0) tpu = 0.0;
    ticks_per_us_ = tpu;
}

float MusicalClock::BeatPhase() const
{
    if (p_quarter_ == 0u) return 0.0f;
    const double t = static_cast<double>(tick_in_beat_) + frac_tick_;
    return static_cast<float>(t / static_cast<double>(p_quarter_));
}

float MusicalClock::BarPhase() const
{
    if (p_bar_ == 0u) return 0.0f;
    const uint64_t into_bar_int = master_tick_ % p_bar_;
    const double   t            = static_cast<double>(into_bar_int) + frac_tick_;
    return static_cast<float>(t / static_cast<double>(p_bar_));
}

float MusicalClock::Bpm() const
{
    if (ticks_per_us_ <= 0.0 || ppqn_ == 0u) return 0.0f;
    return static_cast<float>((ticks_per_us_ * 60.0 * 1e6)
                              / static_cast<double>(ppqn_));
}

void MusicalClock::ApplyDeferredTransport()
{
    if (pending_reset_)
    {
        master_tick_  = 0u;
        frac_tick_    = 0.0;
        bar_          = 0u;
        beat_in_bar_  = 0u;
        tick_in_beat_ = 0u;
        events_ |= CLK_EV::RESET | SubdivisionEventsBetween(0, 0);
        pending_reset_ = false;
    }
    if (pending_seek_)
    {
        SetPosition(pending_seek_tick_);
        frac_tick_ = 0.0;
        have_last_us_ = false;
        pending_seek_ = false;
    }
    if (pending_stop_)
    {
        running_      = false;
        events_      |= CLK_EV::STOP;
        pending_stop_ = false;
    }
    if (pending_start_)
    {
        running_       = true;
        events_       |= CLK_EV::START;
        have_last_us_  = false;
        pending_start_ = false;
    }
}

void MusicalClock::Tick(uint32_t now_us)
{
    events_ = pending_ext_events_;
    pending_ext_events_ = 0u;

    ApplyDeferredTransport();

    if (!running_ || !have_last_us_)
    {
        last_us_      = now_us;
        have_last_us_ = true;
        return;
    }

    uint32_t dt = now_us - last_us_;
    last_us_    = now_us;
    if (dt > kClockMaxTickDtUs) dt = kClockMaxTickDtUs;

    const double elapsed = frac_tick_ + ticks_per_us_ * static_cast<double>(dt);
    if (elapsed < 1.0) { frac_tick_ = elapsed; return; }
    if (!(elapsed < 0x1p64)) return;
    const auto ticks = static_cast<uint64_t>(elapsed);
    frac_tick_ = elapsed - static_cast<double>(ticks);
    const uint64_t next = master_tick_ + ticks;
    // A counter wrap crosses the downbeat, which fires every representable subdivision.
    events_ |= SubdivisionEventsBetween(next < master_tick_ ? 0 : master_tick_ + 1u, next);
    SetPosition(next);
}

bool MusicalClock::Synchronize(uint64_t master_tick, uint32_t now_us, double frac_tick)
{
    if (!std::isfinite(frac_tick) || frac_tick < 0.0 || frac_tick >= 1.0
        || master_tick < master_tick_
        || (master_tick == master_tick_ && frac_tick + 1e-9 < frac_tick_)) return false;
    if (master_tick > master_tick_)
        events_ |= SubdivisionEventsBetween(master_tick_ + 1u, master_tick);
    SetPosition(master_tick);
    frac_tick_ = frac_tick;
    last_us_ = now_us;
    have_last_us_ = true;
    return true;
}

void MusicalClock::SetPosition(uint64_t tick)
{
    master_tick_ = tick;
    const uint64_t beat = tick / ppqn_;
    tick_in_beat_ = static_cast<uint32_t>(tick % ppqn_);
    beat_in_bar_ = static_cast<uint32_t>(beat % beats_per_bar_);
    bar_ = static_cast<uint32_t>(beat / beats_per_bar_);
}

uint32_t MusicalClock::SubdivisionEventsBetween(uint64_t first_tick, uint64_t last_tick) const
{
    uint32_t flags = 0u;
    flags |= Fire(first_tick, last_tick, p_64th_,         CLK_EV::SIXTYFOURTH);
    flags |= Fire(first_tick, last_tick, p_32nd_,         CLK_EV::THIRTYSECOND);
    flags |= Fire(first_tick, last_tick, p_16th_,         CLK_EV::SIXTEENTH);
    flags |= Fire(first_tick, last_tick, p_8th_,          CLK_EV::EIGHTH);
    flags |= Fire(first_tick, last_tick, p_quarter_,      CLK_EV::QUARTER);
    flags |= Fire(first_tick, last_tick, p_half_,         CLK_EV::HALF);
    flags |= Fire(first_tick, last_tick, p_whole_,        CLK_EV::WHOLE);
    flags |= Fire(first_tick, last_tick, p_bar_,          CLK_EV::BAR);
    flags |= Fire(first_tick, last_tick, p_2bar_,         CLK_EV::TWO_BAR);
    flags |= Fire(first_tick, last_tick, p_4bar_,         CLK_EV::FOUR_BAR);
    flags |= Fire(first_tick, last_tick, p_8bar_,         CLK_EV::EIGHT_BAR);

    flags |= Fire(first_tick, last_tick, p_32nd_t_,       CLK_EV::THIRTYSECOND_T);
    flags |= Fire(first_tick, last_tick, p_16th_t_,       CLK_EV::SIXTEENTH_T);
    flags |= Fire(first_tick, last_tick, p_8th_t_,        CLK_EV::EIGHTH_T);
    flags |= Fire(first_tick, last_tick, p_quarter_t_,    CLK_EV::QUARTER_T);
    flags |= Fire(first_tick, last_tick, p_half_t_,       CLK_EV::HALF_T);
    flags |= Fire(first_tick, last_tick, p_whole_t_,      CLK_EV::WHOLE_T);

    flags |= Fire(first_tick, last_tick, p_dotted_16th_,  CLK_EV::DOTTED_16TH);
    flags |= Fire(first_tick, last_tick, p_dotted_8th_,   CLK_EV::DOTTED_8TH);
    flags |= Fire(first_tick, last_tick, p_dotted_qtr_,   CLK_EV::DOTTED_QUARTER);
    flags |= Fire(first_tick, last_tick, p_dotted_half_,  CLK_EV::DOTTED_HALF);
    flags |= Fire(first_tick, last_tick, p_dotted_whole_, CLK_EV::DOTTED_WHOLE);

    flags |= Fire(first_tick, last_tick, p_midi_clock_,   CLK_EV::MIDI_CLOCK);
    return flags;
}

} // namespace alchemy
