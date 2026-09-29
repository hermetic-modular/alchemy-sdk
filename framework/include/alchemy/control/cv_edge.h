/**
 * @file cv_edge.h
 * @brief Hysteresis edge detection for CV inputs — two flavours.
 *
 * Both classes share the same Schmitt+debounce internals; they differ
 * only in defaults tuned to their target signal shape.
 *
 *   alchemy::CvEdge — bipolar pulse / clock-style inputs (rest at 0 V
 *                     ≈ 0.5 on the V1's bipolar-conditioned ADC, swing
 *                     briefly UP to ~+5 V).  Defaults symmetric around
 *                     0.5 (`lo=0.30 / hi=0.70`); primary deliverable
 *                     is the rising-edge bitmask.
 *
 *   alchemy::CvGate — unipolar Eurorack gate inputs (0 V idle, +5 V
 *                     held for the gate's duration).  Defaults sit
 *                     ABOVE 0.5 (`lo=0.55 / hi=0.65`) so the falling
 *                     edge fires cleanly when the input returns to
 *                     ground; rising AND falling edges are first-class.
 *
 * Why both: on V1 hardware (and any module with bipolar ±5 V CV
 * conditioning) 0 V at the jack reads as ~0.5 at `hw.cv[i].Value()`.
 * A clock pulse goes up and briefly returns, so symmetric thresholds
 * around 0.5 are fine — `JustFell()` simply won't fire when the pulse
 * returns to 0 V.  A gate stays high for its duration, so `JustFell()`
 * must fire on release — meaning the LOW threshold has to live above
 * the 0 V rest point.  Using CvEdge's defaults for a gate produces the
 * classic "+5 V triggers, 0 V holds forever, only −5 V releases"
 * failure mode.
 */

#pragma once

#include <cstdint>

namespace alchemy {

constexpr uint8_t kMaxCvEdgeChannels = 8u;

/* ── CvEdge — bipolar pulse / clock-style detection ─────────────────────── */

/**
 * Schmitt+debounce edge detector for **bipolar pulse / clock** inputs.
 * Defaults symmetric around the 0 V rest point (0.5 on V1):
 * `lo=0.30, hi=0.70`.  WRONG for unipolar gates that return to 0 V —
 * see `alchemy::CvGate` below.
 */
class CvEdge
{
  public:
    /**
     * Per-instance configuration — one threshold set covers every
     * channel.  For per-channel thresholds, instantiate multiple CvEdge
     * objects.
     */
    struct Config
    {
        /// Falling-edge threshold (0..1) — CV must drop below this to release.
        /// Default `0.30` is symmetric around the 0 V rest point (0.5), for
        /// **bipolar pulse signals** that swing well below rest.  See
        /// `CvGate` for unipolar gates.
        float    lo_threshold = 0.30f;

        /// Rising-edge threshold (0..1) — CV must rise above this to trigger.
        /// Default `0.70` is symmetric around the 0 V rest point (0.5).
        float    hi_threshold = 0.70f;

        /// Minimum interval between successive rising edges, microseconds.
        /// Edges within this window after the previous rise are ignored.
        uint32_t debounce_us  = 500u;
    };

    /**
     * Initialise for @p num_channels (<= kMaxCvEdgeChannels) with default
     * config.  All channels start low with no recorded edge timestamps.
     */
    void Init(uint8_t num_channels);

    /** Initialise for @p num_channels with explicit @p cfg. */
    void Init(uint8_t num_channels, Config cfg);

    /**
     * Process one tick of CV readings.
     *
     * @param cv      CV readings 0..1, length >= @p num_cv.
     * @param num_cv  Number of channels to read (clamped to Init's num_channels).
     * @param now_us  Current timestamp in microseconds.
     * @return        Bitmask of channels that just rose this tick
     *                (bit n set = channel n transitioned low→high).
     *
     * Falling edges available via `JustFell(ch)` / `FallingMask()`.
     */
    uint32_t Tick(const float* cv, uint8_t num_cv, uint32_t now_us);

    /** Bitmask of channels that just fell during the most recent Tick. */
    uint32_t FallingMask() const { return last_falling_mask_; }

    /** Bitmask of channels that just rose during the most recent Tick. */
    uint32_t RisingMask()  const { return last_rising_mask_; }

    /** Current debounced level for channel @p ch (true = above hi threshold). */
    bool     Level(uint8_t ch) const;

    /** Microsecond timestamp of the most recent rising edge for @p ch. */
    uint32_t LastRiseUs(uint8_t ch) const;

    /** Microsecond timestamp of the most recent falling edge for @p ch. */
    uint32_t LastFallUs(uint8_t ch) const;

    /** Did @p ch rise during the most recent Tick? */
    bool     JustRose(uint8_t ch) const;

    /** Did @p ch fall during the most recent Tick? */
    bool     JustFell(uint8_t ch) const;

    uint8_t  NumChannels() const { return num_channels_; }

  private:
    struct ChannelState
    {
        bool     level        = false;
        bool     just_rose    = false;
        bool     just_fell    = false;
        uint32_t last_rise_us = 0u;
        uint32_t last_fall_us = 0u;
    };

    ChannelState channels_[kMaxCvEdgeChannels] = {};
    Config       cfg_                          = {};
    uint8_t      num_channels_                 = 0u;
    uint32_t     last_rising_mask_             = 0u;
    uint32_t     last_falling_mask_            = 0u;
};

/* ── CvGate — unipolar 0 V↔+5 V gate detection ──────────────────────────── */

/**
 * Schmitt+debounce edge detector for **unipolar Eurorack gate** inputs
 * (0 V off, +5 V on).
 *
 * Both defaults sit ABOVE the 0 V rest point (0.5 on V1) so a gate
 * returning to 0 V cleanly crosses the LOW threshold and releases:
 *
 *   `hi_threshold = 0.65`  ⇄ jack ≈ +1.5 V  (assert)
 *   `lo_threshold = 0.55`  ⇄ jack ≈ +0.5 V  (release)
 *   `debounce_us  = 0`     — gates are typically clean; CvEdge's 500 µs
 *                            default can swallow short gate edges.
 *
 * API is identical to `CvEdge` (forwarded internally); only the
 * defaults differ.  Both rising and falling edges are first-class;
 * `Level(ch)` answers "is the gate currently held?".
 */
class CvGate
{
  public:
    using Config = CvEdge::Config;

    /** Initialise for @p num_channels using gate-appropriate defaults
     *  (`lo=0.55, hi=0.65, debounce_us=0`). */
    void Init(uint8_t num_channels)
    {
        Config cfg;
        cfg.lo_threshold = 0.55f;
        cfg.hi_threshold = 0.65f;
        cfg.debounce_us  = 0u;
        edge_.Init(num_channels, cfg);
    }

    /** Initialise for @p num_channels with explicit @p cfg. */
    void Init(uint8_t num_channels, Config cfg) { edge_.Init(num_channels, cfg); }

    uint32_t Tick(const float* cv, uint8_t num_cv, uint32_t now_us)
    {
        return edge_.Tick(cv, num_cv, now_us);
    }

    uint32_t FallingMask() const            { return edge_.FallingMask(); }
    uint32_t RisingMask()  const            { return edge_.RisingMask();  }
    bool     Level     (uint8_t ch) const   { return edge_.Level(ch);     }
    uint32_t LastRiseUs(uint8_t ch) const   { return edge_.LastRiseUs(ch);}
    uint32_t LastFallUs(uint8_t ch) const   { return edge_.LastFallUs(ch);}
    bool     JustRose  (uint8_t ch) const   { return edge_.JustRose(ch);  }
    bool     JustFell  (uint8_t ch) const   { return edge_.JustFell(ch);  }
    uint8_t  NumChannels() const            { return edge_.NumChannels(); }

  private:
    CvEdge edge_;
};

} // namespace alchemy
