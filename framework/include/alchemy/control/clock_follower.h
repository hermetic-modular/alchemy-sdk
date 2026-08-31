/**
 * @file clock_follower.h
 * @brief External-clock PI-PLL that steers `alchemy::MusicalClock`.
 *
 * `ClockFollower` consumes external clock pulses (Eurorack CV @ 24 PPQN,
 * MIDI 0xF8, tap, etc.) via `OnPulse(stamp_us)` and steers the NCO's
 * rate via `MusicalClock::SetTicksPerUs`.  Pulses *never* perturb NCO
 * phase directly — the timeline stays monotonic.  See spec §4 and §2.
 *
 * Wiring inside ControlLoop::OnPoll:
 *
 *     clk_edge_.Tick(cv, num_cv, now_us);
 *     if (clk_edge_.JustRose(kClockChan))
 *         follower_.OnPulse(clk_edge_.LastRiseUs(kClockChan));
 *     follower_.Update(now_us);
 *     clock_.Tick(now_us);
 *
 * Phase error is measured at pulse-stamp time (anchored to
 * `MusicalClock::LastTickUs`), so the `Update` / `Tick` order is not
 * load-bearing.  `OnPulse` is ISR-safe against the poll path via a
 * single-producer ring: safe from an EXTI or UART ISR, one pulse source
 * at a time.
 */

#pragma once

#include <cstdint>

#include "alchemy/control/musical_clock.h"

namespace alchemy {

/* ── Tuning constants (values from clock bring-up; see musical_clock.h) ── */

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

/**
 * What the follower does to the NCO when the external clock is lost.
 *
 * The "right" answer depends on the module type — an effect generally
 * wants to keep timing through a brief dropout (`Freewheel`); a sequencer
 * wants to halt so it doesn't drift out of sync (`Freeze`) or hard-stop
 * (`Stop`).  Hardcoding one policy would repeat the `ClockPll` mistake
 * (baked-in 24 PPQN) that this layer is meant to fix.  Default is
 * `Freewheel`.
 */
enum class ClockLossPolicy : uint8_t
{
    Freewheel = 0,   ///< Keep last rate; NCO keeps integrating in time.
    Freeze    = 1,   ///< Hold position; NCO stops advancing until re-lock.
    Stop      = 2,   ///< Unlock + halt transport via `MusicalClock::Stop`.
};

/* ── ClockFollower ──────────────────────────────────────────────────────── */

class ClockFollower
{
  public:
    explicit ClockFollower(MusicalClock& nco) : nco_(&nco) {}

    /* ── Source selection ───────────────────────────────────────────── */

    /** Take over rate steering.  Stops the NCO and zeroes its rate so it
     *  does not free-run at the old internal tempo while waiting for the
     *  first lock (azoth `extclock_enable`). */
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

    /** Deposit a rising-edge timestamp (µs).  Cheap; ISR-safe against the
     *  poll path.  Pulses are ring-buffered so a poll delayed past a
     *  pulse period (fast clocks + frame work) drops nothing.  Single
     *  producer: wire one pulse source at a time. */
    void OnPulse(uint32_t stamp_us);

    /** Per-poll work: drain buffered pulses, update the period EMA, run
     *  the PI controller, detect loss.  Call once per ~1 ms poll. */
    void Update(uint32_t now_us);

    /** Re-anchor the phase reference after the NCO has been reset to the
     *  downbeat.  Call alongside `MusicalClock::Reset()` so the follower
     *  does not fight the discontinuity (spec §4.8). */
    void OnReset();

    /* ── Queries ─────────────────────────────────────────────────────── */

    /** True after `kClockLockPulses` consecutive valid pulses. */
    bool Locked() const { return locked_; }

    /** Estimated source tempo (BPM); 0 while no period estimate exists. */
    float Bpm() const;

    /* ── Advanced retuning ───────────────────────────────────────────── */

    /** Override the shipped tuning constants.  Most users should not. */
    void SetGains(float kp, float ki, float ema_alpha);

  private:
    bool   ConsumePulse(uint32_t stamp_us);
    double FracTickAt(uint32_t stamp_us) const;
    void   RecomputeLimits();
    void   HandleLoss(uint32_t now_us);
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

    /* ── ISR ↔ poll handoff: SPSC ring, ISR owns head, `Update` owns
     *    tail (free-running indices).  8 deep buys 66 ms of poll stall
     *    at kClockBpmMax / 24 PPQN — beyond `kClockMaxTickDtUs`. ────── */
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
