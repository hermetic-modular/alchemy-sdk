#include "alchemy/control/clock_follower.h"

#include <cassert>

namespace alchemy {

namespace {

inline double Clamp(double v, double lo, double hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

} // namespace

/* ── Source selection ──────────────────────────────────────────────────── */

void ClockFollower::Enable(uint32_t ext_ppqn)
{
    assert(nco_ != nullptr);
    assert(ext_ppqn > 0u);
    assert(nco_->Ppqn() % ext_ppqn == 0u);

    ext_ppqn_          = ext_ppqn;
    enabled_           = true;
    locked_            = false;
    have_last_stamp_   = false;
    valid_pulse_count_ = 0u;
    period_est_us_     = 0.0;
    integrator_        = 0.0;
    loss_announced_    = false;
    ring_tail_         = ring_head_; /* discard pulses from before Enable */

    nco_->SetSteered(true);
    nco_->Stop();
    nco_->SetTicksPerUs(0.0);

    RecomputeLimits();
}

void ClockFollower::Disable()
{
    enabled_        = false;
    locked_         = false;
    loss_announced_ = false;
    if (nco_ != nullptr)
        nco_->SetSteered(false);
}

void ClockFollower::SetExtPpqn(uint32_t ppqn)
{
    assert(ppqn > 0u);
    assert(nco_ != nullptr && nco_->Ppqn() % ppqn == 0u);

    ext_ppqn_          = ppqn;
    locked_            = false;
    have_last_stamp_   = false;
    valid_pulse_count_ = 0u;
    integrator_        = 0.0;
    /* period_est preserved — same wall-clock pulses, just different
     * musical meaning. */
    RecomputeLimits();
}

void ClockFollower::RecomputeLimits()
{
    const double fastest_q_us     = (60.0 / kClockBpmMax) * 1e6;
    const double fastest_pulse_us = fastest_q_us / static_cast<double>(ext_ppqn_);
    uint32_t     min_us           = static_cast<uint32_t>(fastest_pulse_us * 0.25);
    if (min_us < kClockMinPeriodFloorUs) min_us = kClockMinPeriodFloorUs;
    min_period_us_ = min_us;

    const double slowest_q_us     = (60.0 / kClockBpmMin) * 1e6;
    const double slowest_pulse_us = slowest_q_us / static_cast<double>(ext_ppqn_);
    uint32_t     max_us           = static_cast<uint32_t>(slowest_pulse_us * 2.0);
    if (max_us < kClockMaxPeriodFloorUs) max_us = kClockMaxPeriodFloorUs;
    max_period_us_ = max_us;
}

void ClockFollower::SetGains(float kp, float ki, float ema_alpha)
{
    kp_        = kp;
    ki_        = ki;
    ema_alpha_ = ema_alpha;
}

float ClockFollower::Bpm() const
{
    if (period_est_us_ <= 0.0) return 0.0f;
    return static_cast<float>(
        60.0 / (period_est_us_ * static_cast<double>(ext_ppqn_) * 1e-6));
}

void ClockFollower::OnReset()
{
    last_pulse_tick_ = 0u;
    last_pulse_frac_ = 0.0;
    integrator_      = 0.0;
}

void ClockFollower::OnSeek()
{
    last_pulse_tick_ = nco_->MasterTick();
    last_pulse_frac_ = nco_->FracTick();
    integrator_ = 0.0;
    if (locked_) SeedRateFromPeriod();
}

/* ── Pulse intake ──────────────────────────────────────────────────────── */

void ClockFollower::OnPulse(uint32_t stamp_us)
{
    if (!enabled_) return;
    const uint32_t head = ring_head_;
    if (head - ring_tail_ >= kPulseRingSize) return;
    pulse_ring_[head % kPulseRingSize] = stamp_us;
    ring_head_ = head + 1u;
}

/* Per-pulse intake: seeding, validity window, period EMA, lock
 * acquisition.  True ⇒ the pulse extended the valid stream. */
bool ClockFollower::ConsumePulse(uint32_t stamp_us)
{
    if (!have_last_stamp_)
    {
        last_pulse_stamp_  = stamp_us;
        have_last_stamp_   = true;
        valid_pulse_count_ = 1u;
        loss_announced_    = false;
        return false;
    }

    const uint32_t dt = stamp_us - last_pulse_stamp_;

    if (dt < min_period_us_)
        return false; /* glitch — ignore */

    if (dt > max_period_us_)
    {
        // Use this pulse as the first observation of a new tempo.
        locked_            = false;
        loss_announced_    = false;
        valid_pulse_count_ = 1u;
        last_pulse_stamp_  = stamp_us;
        period_est_us_     = 0.0;
        integrator_        = 0.0;
        return false;
    }

    const double dt_d = static_cast<double>(dt);
    if (period_est_us_ <= 0.0)
        period_est_us_ = dt_d;
    else
        period_est_us_ += static_cast<double>(ema_alpha_)
                       * (dt_d - period_est_us_);

    last_pulse_stamp_ = stamp_us;
    ++valid_pulse_count_;
    loss_announced_   = false;

    if (!locked_ && valid_pulse_count_ >= kClockLockPulses)
        DeclareLock(stamp_us);

    return true;
}

/* ── Per-poll work ─────────────────────────────────────────────────────── */

void ClockFollower::Update(uint32_t now_us)
{
    if (!enabled_ || nco_ == nullptr) return;

    /* ── Drain the ring (bounded by the head snapshot).  `accepted`
     *    counts valid pulses consumed while already locked — the PI's
     *    expected NCO travel scales with it. */
    const uint32_t head     = ring_head_;
    uint32_t       accepted = 0u;
    for (uint32_t tail = ring_tail_; tail != head;)
    {
        const uint32_t stamp = pulse_ring_[tail % kPulseRingSize];
        ring_tail_           = ++tail;

        const bool was_locked = locked_;
        if (ConsumePulse(stamp) && was_locked)
            ++accepted;
    }

    /* ── Loss detection.  After the drain so buffered pulses count as
     *    heard; signed because a pulse can stamp after `now_us` was
     *    sampled at the top of the poll. */
    if (have_last_stamp_ && period_est_us_ > 0.0 && !loss_announced_)
    {
        const int32_t dt_since =
            static_cast<int32_t>(now_us - last_pulse_stamp_);
        const double thresh =
            period_est_us_ * static_cast<double>(kClockLossFactor);
        if (dt_since > 0 && static_cast<double>(dt_since) > thresh)
        {
            HandleLoss();
            return;
        }
    }

    if (!locked_ || accepted == 0u)
        return;

    /* ── PI controller (post-lock).  Both ends of the phase delta are
     *    taken at pulse-stamp time (`FracTickAt`), so drain lateness
     *    cancels out of the phase error. */
    const double ticks_per_pulse =
        static_cast<double>(nco_->Ppqn()) / static_cast<double>(ext_ppqn_);

    const uint64_t mtick         = nco_->MasterTick();
    const double   frac_at_stamp = FracTickAt(last_pulse_stamp_);

    const double delta = static_cast<double>(mtick - last_pulse_tick_)
                       + (frac_at_stamp - last_pulse_frac_);
    const double phase_err =
        static_cast<double>(accepted) * ticks_per_pulse - delta;

    integrator_ += phase_err;
    integrator_  = Clamp(integrator_,
                         -static_cast<double>(kClockPllIntegratorMax),
                          static_cast<double>(kClockPllIntegratorMax));

    const double base_per_sec = (static_cast<double>(nco_->Ppqn()) * 1e6)
                              / (static_cast<double>(ext_ppqn_) * period_est_us_);
    const double correction   = static_cast<double>(kp_) * phase_err
                              + static_cast<double>(ki_) * integrator_;
    double rate_per_sec       = base_per_sec + correction;

    /* Safety clamp on absolute rate.  The PI loop should never demand
     * a tempo beyond `kClockBpmMax`. */
    const double max_per_sec = (static_cast<double>(kClockBpmMax)
                                * static_cast<double>(nco_->Ppqn())) / 60.0;
    rate_per_sec = Clamp(rate_per_sec, 0.0, max_per_sec);

    nco_->SetTicksPerUs(rate_per_sec * 1e-6);

    last_pulse_tick_ = mtick;
    last_pulse_frac_ = frac_at_stamp;
}

/* ── State transitions ─────────────────────────────────────────────────── */

/* NCO fractional position extrapolated from the last `Tick` to a pulse
 * stamp; exact while the rate is constant across the span. */
double ClockFollower::FracTickAt(uint32_t stamp_us) const
{
    const int32_t span_us =
        static_cast<int32_t>(stamp_us - nco_->LastTickUs());
    return nco_->FracTick()
         + nco_->TicksPerUs() * static_cast<double>(span_us);
}

void ClockFollower::DeclareLock(uint32_t stamp_us)
{
    locked_ = true;

    /* Anchor before seeding: the extrapolation in AnchorPhase must use
     * the rate the NCO has been integrating with. */
    AnchorPhase(stamp_us);
    SeedRateFromPeriod();
    integrator_ = 0.0;

    nco_->OrPendingEvent(CLK_EV::EXT_LOCK);

    if (auto_start_)
        nco_->Start();
}

void ClockFollower::SeedRateFromPeriod()
{
    if (period_est_us_ <= 0.0)
        return;
    const double rate_per_sec = (static_cast<double>(nco_->Ppqn()) * 1e6)
                              / (static_cast<double>(ext_ppqn_) * period_est_us_);
    const double max_per_sec = static_cast<double>(kClockBpmMax) * nco_->Ppqn() / 60.0;
    nco_->SetTicksPerUs(Clamp(rate_per_sec, 0.0, max_per_sec) * 1e-6);
}

void ClockFollower::AnchorPhase(uint32_t stamp_us)
{
    last_pulse_tick_ = nco_->MasterTick();
    last_pulse_frac_ = FracTickAt(stamp_us);
}

void ClockFollower::HandleLoss()
{
    /* All policies share: forget the pulse counter and the last-stamp
     * anchor (so the next pulse re-enters lock acquisition), preserve
     * `period_est` for warm re-lock, fire EXT_LOST once. */
    locked_            = false;
    have_last_stamp_   = false;
    valid_pulse_count_ = 0u;
    integrator_        = 0.0;
    loss_announced_    = true;

    nco_->OrPendingEvent(CLK_EV::EXT_LOST);

    switch (loss_policy_)
    {
        case ClockLossPolicy::Freewheel:
            break;
        case ClockLossPolicy::Freeze:
            nco_->SetTicksPerUs(0.0);
            break;
        case ClockLossPolicy::Stop:
            nco_->Stop();
            break;
    }
}

} // namespace alchemy
