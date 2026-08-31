/**
 * @file musical_clock.h
 * @brief Free-running, monotonic musical timeline (NCO).
 *
 * `MusicalClock` is a numerically-controlled oscillator that integrates a
 * **rate** every control tick and emits musical position (bar / beat /
 * tick-in-beat / continuous phase) plus a per-tick subdivision event
 * bitmask (`CLK_EV_*`).  It is the sole source of truth that modules read.
 *
 * It does not know or care whether its rate comes from an internal BPM
 * setting (`SetBpm`) or an external follower (`alchemy::ClockFollower`).
 *
 * Phase advances continuously between control polls (time-based
 * integration; no fixed-rate assumption) and is **monotonic** — it never
 * moves backward.  Audio modules can therefore derive sample-accurate
 * gates and playheads from `BeatPhase()` / `BarPhase()` without clicks.
 *
 * Typical wiring inside ControlLoop::OnPoll, once per ~1 ms tick:
 *
 *     follower_.Update(now_us);   // PI loop, loss detection
 *     clock_.Tick(now_us);        // integrate rate → events + position
 *
 * Heap-free, ISR-safe to write `SetTicksPerUs` /  `OrPendingEvent` from
 * the poll context (single-word stores on Cortex-M7).
 */

#pragma once

#include <cstdint>

namespace alchemy {

/* ── Constants ─────────────────────────────────────────────────────────── */

/** Internal timeline resolution.  Divisible by 24, 16, 12, 8, 6, 4, 3, 2 so
 *  every straight, triplet, and dotted subdivision lands on an exact
 *  integer tick. */
constexpr uint32_t kClockPpqnInternal = 96u;

/** Standard MIDI / Eurorack external clock (used by `ClockFollower`). */
constexpr uint32_t kClockExtPpqnDefault = 24u;

/** Default beats per bar (4/4). */
constexpr uint8_t kClockBeatsPerBarDefault = 4u;

/** Tempo bounds shared by the NCO rate clamp and the follower validity
 *  window.  Single source of truth — do not duplicate. */
constexpr float kClockBpmMin = 20.0f;
constexpr float kClockBpmMax = 300.0f;

/** Catch-up clamp on the per-tick dt.  Limits how many sub-ticks a long
 *  poll stall can produce in a single `Tick`.  50 ms covers any plausible
 *  scheduler hiccup; longer stalls would dump a burst of catch-up ticks. */
constexpr uint32_t kClockMaxTickDtUs = 50000u;

/* ── Subdivision event bitmask (`CLK_EV_*`) ─────────────────────────────── */

namespace CLK_EV {

/* Transport */
constexpr uint32_t RESET           = 1u <<  0;
constexpr uint32_t START           = 1u <<  1;
constexpr uint32_t STOP            = 1u <<  2;

/* Straight */
constexpr uint32_t SIXTYFOURTH     = 1u <<  3;
constexpr uint32_t THIRTYSECOND    = 1u <<  4;
constexpr uint32_t SIXTEENTH       = 1u <<  5;
constexpr uint32_t EIGHTH          = 1u <<  6;
constexpr uint32_t QUARTER         = 1u <<  7;
constexpr uint32_t HALF            = 1u <<  8;
constexpr uint32_t WHOLE           = 1u <<  9;
constexpr uint32_t BAR             = 1u << 10;

/* Triplet */
constexpr uint32_t THIRTYSECOND_T  = 1u << 11;
constexpr uint32_t SIXTEENTH_T     = 1u << 12;
constexpr uint32_t EIGHTH_T        = 1u << 13;
constexpr uint32_t QUARTER_T       = 1u << 14;
constexpr uint32_t HALF_T          = 1u << 15;
constexpr uint32_t WHOLE_T         = 1u << 16;

/* Dotted */
constexpr uint32_t DOTTED_16TH     = 1u << 17;
constexpr uint32_t DOTTED_8TH      = 1u << 18;
constexpr uint32_t DOTTED_QUARTER  = 1u << 19;
constexpr uint32_t DOTTED_HALF     = 1u << 20;
constexpr uint32_t DOTTED_WHOLE    = 1u << 21;

/* Multi-bar */
constexpr uint32_t TWO_BAR         = 1u << 22;
constexpr uint32_t FOUR_BAR        = 1u << 23;
constexpr uint32_t EIGHT_BAR       = 1u << 24;

/* Clock-grid (24 PPQN) — kept distinct from THIRTYSECOND_T (same period at
 * 96 PPQN) so a future internal-PPQN change cannot silently break MIDI
 * clock emission. */
constexpr uint32_t MIDI_CLOCK      = 1u << 25;

/* External-clock state (set by `ClockFollower` via `OrPendingEvent`). */
constexpr uint32_t EXT_LOCK        = 1u << 26;
constexpr uint32_t EXT_LOST        = 1u << 27;

} // namespace CLK_EV

/* ── MusicalClock ──────────────────────────────────────────────────────── */

class MusicalClock
{
  public:
    /**
     * @param ppqn          Internal timeline resolution.  Must be a multiple
     *                      of 24 (so every straight, triplet, dotted, and
     *                      24-PPQN MIDI-clock subdivision lands on exact
     *                      integer ticks).  Default 96 is recommended.
     * @param beats_per_bar Time-signature numerator.  Only the bar /
     *                      multi-bar event math depends on it.
     */
    explicit MusicalClock(uint32_t ppqn          = kClockPpqnInternal,
                          uint8_t  beats_per_bar = kClockBeatsPerBarDefault);

    /* ── Per-poll integration ────────────────────────────────────────── */

    /** Apply any deferred transport, integrate `ticks_per_us * dt`, and
     *  fire subdivision events for every integer tick that crossed.
     *  Call once per control poll (≈ 1 ms).  `now_us` is a microsecond
     *  timestamp from the platform monotonic counter (32-bit wrap-safe
     *  internally). */
    void Tick(uint32_t now_us);

    /* ── Transport (deferred; applied at the top of the next Tick) ───── */

    /** Begin advancing.  Fires `CLK_EV::START` on the next `Tick`. */
    void Start();

    /** Halt advancement, retaining position.  Fires `CLK_EV::STOP`. */
    void Stop();

    /** Snap to the downbeat (master_tick = 0, frac_tick = 0).  Fires
     *  `CLK_EV::RESET` and all downbeat subdivision flags. */
    void Reset();

    /* ── Configuration ───────────────────────────────────────────────── */

    /** Set internal tempo.  Ignored while a follower is steering (see
     *  `IsSteered`); user-side gating is unnecessary — the no-op is safe. */
    void SetBpm(float bpm);

    /** Change the bar length (time-signature numerator). */
    void SetTimeSignature(uint8_t beats_per_bar);

    /* ── Rate-control surface used by ClockFollower ──────────────────── */

    /** Write the steered rate in internal ticks per microsecond.  Cheap;
     *  safe to call from the poll path.  A follower should always call
     *  `SetSteered(true)` first so user `SetBpm` calls become no-ops. */
    void SetTicksPerUs(double tpu);

    /** Current rate (internal ticks / µs). */
    double TicksPerUs() const { return ticks_per_us_; }

    /** Mark the NCO as being steered by an external source (suppresses
     *  `SetBpm`).  Called by `ClockFollower::Enable` / `Disable`. */
    void SetSteered(bool on) { steered_ = on; }

    /** True iff an external source is currently steering the rate. */
    bool IsSteered() const { return steered_; }

    /** OR additional flags into the next `Tick`'s event bitmask.  Intended
     *  for the follower to inject `EXT_LOCK` / `EXT_LOST` so they coalesce
     *  with the same Tick's subdivision flags.  Cheap, ISR-safe. */
    void OrPendingEvent(uint32_t flags) { pending_ext_events_ |= flags; }

    /* ── Queries ─────────────────────────────────────────────────────── */

    /** `CLK_EV_*` bitmask for the most recent `Tick` batch. */
    uint32_t Events() const { return events_; }

    /** True between `Start` and `Stop`. */
    bool     Running() const { return running_; }

    uint32_t Bar() const { return bar_; }
    uint32_t Beat() const { return beat_in_bar_; }
    uint32_t TickInBeat() const { return tick_in_beat_; }
    uint64_t MasterTick() const { return master_tick_; }
    uint32_t Ppqn() const { return ppqn_; }
    uint8_t  BeatsPerBar() const { return beats_per_bar_; }

    /** Sub-tick residual [0, 1).  Exposed so a follower can compute
     *  sub-tick phase error: `delta = (master - anchor_tick) + (frac - anchor_frac)`. */
    double   FracTick() const { return frac_tick_; }

    uint32_t LastTickUs() const { return last_us_; }

    /** Continuous position within the current beat, [0, 1). */
    float BeatPhase() const;

    /** Continuous position within the current bar, [0, 1). */
    float BarPhase() const;

    /** Derived display tempo (BPM).  0 if no rate set yet. */
    float Bpm() const;

  private:
    void ApplyDeferredTransport();
    void AdvanceOnePpqnTick();
    void RecomputePeriods();
    void FireDownbeatEvents();

    /* ── Position / rate ─────────────────────────────────────────────── */
    uint64_t master_tick_   = 0u;
    double   frac_tick_     = 0.0;
    double   ticks_per_us_  = 0.0;
    uint32_t bar_           = 0u;
    uint32_t beat_in_bar_   = 0u;
    uint32_t tick_in_beat_  = 0u;
    uint32_t events_        = 0u;
    bool     running_       = false;
    bool     steered_       = false;

    /* ── Configuration ───────────────────────────────────────────────── */
    uint32_t ppqn_          = kClockPpqnInternal;
    uint8_t  beats_per_bar_ = kClockBeatsPerBarDefault;

    /* ── Timekeeping ─────────────────────────────────────────────────── */
    uint32_t last_us_       = 0u;
    bool     have_last_us_  = false;

    /* ── Deferred transport / external events ────────────────────────── */
    bool     pending_start_     = false;
    bool     pending_stop_      = false;
    bool     pending_reset_     = false;
    uint32_t pending_ext_events_ = 0u;

    /* ── Pre-computed event periods (0 = "not representable at this PPQN") */
    uint32_t p_64th_        = 0u;
    uint32_t p_32nd_        = 0u;
    uint32_t p_16th_        = 0u;
    uint32_t p_8th_         = 0u;
    uint32_t p_quarter_     = 0u;
    uint32_t p_half_        = 0u;
    uint32_t p_whole_       = 0u;
    uint32_t p_bar_         = 0u;
    uint32_t p_2bar_        = 0u;
    uint32_t p_4bar_        = 0u;
    uint32_t p_8bar_        = 0u;
    uint32_t p_32nd_t_      = 0u;
    uint32_t p_16th_t_      = 0u;
    uint32_t p_8th_t_       = 0u;
    uint32_t p_quarter_t_   = 0u;
    uint32_t p_half_t_      = 0u;
    uint32_t p_whole_t_     = 0u;
    uint32_t p_dotted_16th_ = 0u;
    uint32_t p_dotted_8th_  = 0u;
    uint32_t p_dotted_qtr_  = 0u;
    uint32_t p_dotted_half_ = 0u;
    uint32_t p_dotted_whole_= 0u;
    uint32_t p_midi_clock_  = 0u;
};

} // namespace alchemy
