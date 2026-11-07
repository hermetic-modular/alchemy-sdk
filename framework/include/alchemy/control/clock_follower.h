#pragma once

#include <cstdint>

#include "alchemy/control/musical_clock.h"

namespace alchemy {

/* ── PLL tuning ──────────────────────────────────────────────────────── */

/** EMA smoothing for the period estimator. */
constexpr float    kClockEmaAlpha          = 0.15f;

/** PI proportional gain (ticks-of-correction per tick-of-phase-error). */
constexpr float    kClockPllKp             = 2.0f;

/** PI integral gain. */
constexpr float    kClockPllKi             = 0.05f;

/** Anti-windup clamp on the integrator (± ticks). */
constexpr float    kClockPllIntegratorMax  = 48.0f;

/** Consecutive valid pulses required to declare lock. */
constexpr uint32_t kClockLockPulses        = 3u;

/** Loss factor: × period_est with no pulse ⇒ external clock lost. */
constexpr float    kClockLossFactor        = 3.0f;

/** Floor for the glitch-rejection minimum period (µs). */
constexpr uint32_t kClockMinPeriodFloorUs  = 500u;

/** Floor for the slow-clock maximum period (µs). */
constexpr uint32_t kClockMaxPeriodFloorUs  = 100000u;

/* ── Loss policy ────────────────────────────────────────────────────────── */

enum class ClockLossPolicy : uint8_t
{
    Freewheel = 0,   ///< Keep last rate; NCO keeps integrating in time.
    Freeze    = 1,   ///< Hold position; NCO stops advancing until re-lock.
    Stop      = 2,   ///< Unlock + halt transport via `MusicalClock::Stop`.
};

// Steers the clock rate from external pulses measured in MusicalClock's microsecond epoch.
class ClockFollower
{
  public:
    explicit ClockFollower(MusicalClock& nco) : nco_(&nco) {}

    /* ── Source selection ───────────────────────────────────────────── */

    /** Take over rate steering.  Stops the NCO and zeroes its rate so it
     *  does not free-run at the old internal tempo while waiting for the
     *  first lock. */
    void Enable(uint32_t ext_ppqn = kClockExtPpqnDefault);

    /** Release rate steering.  The NCO keeps its current rate; the user
     *  may follow with `SetBpm` to resume internal-clock operation. */
    void Disable();

    /** True while the follower is enabled (steering the NCO). */
    bool Enabled() const { return enabled_; }

    /** Change the assumed external PPQN.  Must divide the NCO's internal
     *  PPQN so `ticks_per_pulse` stays integer.  Triggers a soft re-lock. */
    void SetExtPpqn(uint32_t ppqn);

    /** External PPQN currently assumed (24 by default). */
    uint32_t ExtPpqn() const { return ext_ppqn_; }

    /** What to do when the external clock disappears (default Freewheel). */
    void SetLossPolicy(ClockLossPolicy p) { loss_policy_ = p; }

    /** If true, the NCO transport is auto-`Start()`ed on first lock. */
    void SetAutoStart(bool on) { auto_start_ = on; }

    /* ── Pulse intake ────────────────────────────────────────────────── */

    /** One producer may enqueue pulses from an ISR. A full ring drops the new pulse. */
    void OnPulse(uint32_t stamp_us);

    /** Per-poll work: drain buffered pulses, update the period EMA, run
     *  the PI controller, detect loss.  Call once per ~1 ms poll. */
    void Update(uint32_t now_us);

    /** Anchor to the downbeat when resetting the clock. */
    void OnReset();
    /** Anchor to the clock's current position after a seek or phase correction. */
    void OnSeek();

    /* ── Queries ─────────────────────────────────────────────────────── */

    /** True after `kClockLockPulses` consecutive valid pulses. */
    bool Locked() const { return locked_; }

    /** Estimated source tempo (BPM); 0 while no period estimate exists. */
    float Bpm() const;

    /* ── Advanced retuning ───────────────────────────────────────────── */

    void SetGains(float kp, float ki, float ema_alpha);

  private:
    bool   ConsumePulse(uint32_t stamp_us);
    double FracTickAt(uint32_t stamp_us) const;
    void   RecomputeLimits();
    void   HandleLoss();
    void   AnchorPhase(uint32_t stamp_us);
    void   DeclareLock(uint32_t stamp_us);
    void   SeedRateFromPeriod();

    MusicalClock* nco_;

    /* ── Configuration ──────────────────────────────────────────────── */
    bool            enabled_     = false;
    uint32_t        ext_ppqn_    = kClockExtPpqnDefault;
    ClockLossPolicy loss_policy_ = ClockLossPolicy::Freewheel;
    bool            auto_start_  = false;

    /* ── Tuning (mutable so a user can call SetGains at runtime) ─────── */
    float kp_         = kClockPllKp;
    float ki_         = kClockPllKi;
    float ema_alpha_  = kClockEmaAlpha;

    // OnPulse owns the head. Update owns the tail.
    static constexpr uint32_t kPulseRingSize = 8u;
    static_assert((kPulseRingSize & (kPulseRingSize - 1u)) == 0u,
                  "free-running ring indices need a power-of-two size");
    volatile uint32_t pulse_ring_[kPulseRingSize] = {};
    volatile uint32_t ring_head_ = 0u;
    volatile uint32_t ring_tail_ = 0u;

    /* ── State ──────────────────────────────────────────────────────── */
    bool     locked_            = false;
    bool     have_last_stamp_   = false;
    uint32_t last_pulse_stamp_  = 0u;
    uint32_t valid_pulse_count_ = 0u;
    double   period_est_us_     = 0.0;
    double   integrator_        = 0.0;
    uint64_t last_pulse_tick_   = 0u;   /* NCO position at the last     */
    double   last_pulse_frac_   = 0.0;  /* accepted pulse's stamp time  */
    bool     loss_announced_    = false;

    /* ── Validity window (µs) ───────────────────────────────────────── */
    uint32_t min_period_us_ = kClockMinPeriodFloorUs;
    uint32_t max_period_us_ = kClockMaxPeriodFloorUs;
};

} // namespace alchemy
