/**
 * @file pot_catch.h
 * @brief Generic pot-catch state and algorithm.
 *
 * Pot-catch prevents a parameter jump when the user switches to a page
 * where the stored value differs from the physical pot position.  The
 * pot is "uncaught" until it physically passes through the stored value
 * (crossover catch) or lands within kCatchTolerance of it (proximity
 * catch).
 *
 * PotState and ParamLockSlot are plain data structs; InitCatch and
 * UpdateCatch are stateless algorithms that operate on them.  They
 * carry no hardware or application dependencies.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace alchemy {

/* ── Tuning constants ─────────────────────────────────────────────────── */

/** Proximity window: pot is caught immediately if within this distance. */
constexpr float    kCatchTolerance             = 0.005f;

/** Minimum physical nudge (0..1) required to start / stop param-lock. */
constexpr float    kParamLockGestureThreshold  = 0.03f;

/** Motion gate: travel off a sleeping stored value required to wake. */
constexpr float    kPotWakeThreshold           = 0.004f;

/** Motion gate: stillness band around the awake anchor. */
constexpr float    kPotStillBand               = 0.0015f;

/** Motion gate: stillness time before an awake pot sleeps. */
constexpr uint32_t kPotSleepMs                 = 400u;

/* ── PotState ─────────────────────────────────────────────────────────── */

/**
 * Per-pot catch state.
 *
 * stored       Logical parameter value (0..1) visible to the engine.
 * caught       True once the physical position has caught the stored value.
 * sign_at_lock Sign of (phys - stored) at the moment catch was disabled;
 *              used for crossover detection.
 *
 * anchor / last_move_ms / awake — motion-gate state, used only by the
 * timed UpdateCatch overload.
 */
struct PotState
{
    float stored       = 0.5f;
    bool  caught       = true;
    int   sign_at_lock = 0;

    float    anchor       = 0.5f;
    uint32_t last_move_ms = 0u;
    bool     awake        = false;
};


struct ParamLockSlot
{
    bool     active      = false;
    bool     recording   = false;
    uint16_t length      = 0;      ///< samples recorded (≤ stride)
    uint32_t play_pos    = 0;      ///< Q16.16 play position (see above)
    uint16_t record_base = 0;      ///< quantized pot position at arm time
    float    rec_accum   = 0.0f;   ///< box-average accumulator (record)
    uint16_t rec_count   = 0;      ///< frames accumulated toward next sample
};

/* The atomicity argument above requires natural alignment. */
static_assert(offsetof(ParamLockSlot, play_pos) % alignof(uint32_t) == 0,
              "ParamLockSlot::play_pos must be 4-byte aligned — the audio "
              "ISR relies on its store being single-copy atomic");

/* ── Algorithms ───────────────────────────────────────────────────────── */

/**
 * (Re-)initialise catch state when a pot's stored value changes externally
 * (page switch, tap tempo, preset load).  The pot becomes uncaught if the
 * physical position is outside the tolerance window, recording the current
 * sign so the crossover path can catch it too.
 */
inline void InitCatch(PotState& s, float phys)
{
    const float diff = phys - s.stored;
    if (diff > -kCatchTolerance && diff < kCatchTolerance)
    {
        s.caught       = true;
        s.sign_at_lock = 0;
        s.stored       = phys;
    }
    else
    {
        s.caught       = false;
        s.sign_at_lock = (diff > 0.0f) ? 1 : -1;
    }
    /* Gate parks asleep: the value holds until genuine motion. */
    s.awake  = false;
    s.anchor = phys;
}

/** Untimed legacy update — raw follow while caught, no motion gate. */
inline void UpdateCatch(PotState& s, float phys)
{
    if (s.caught)
    {
        s.stored = phys;
        return;
    }

    const float diff = phys - s.stored;
    if (diff > -kCatchTolerance && diff < kCatchTolerance)
    {
        s.caught = true;
        s.stored = phys;
        return;
    }

    const int cur_sign = (diff > 0.0f) ? 1 : -1;
    if (s.sign_at_lock != 0 && cur_sign != s.sign_at_lock)
    {
        s.caught = true;
        s.stored = phys;
    }
}

/**
 * Timed update with a motion gate.  Caught + asleep holds stored
 * bit-exact; waking takes > kPotWakeThreshold of travel; awake tracks
 * phys 1:1 and sleeps after kPotSleepMs inside kPotStillBand.
 */
inline void UpdateCatch(PotState& s, float phys, uint32_t t_ms)
{
    if (s.caught)
    {
        if (!s.awake)
        {
            const float d = phys - s.stored;
            if (d > kPotWakeThreshold || d < -kPotWakeThreshold)
            {
                s.awake        = true;
                s.anchor       = phys;
                s.last_move_ms = t_ms;
                s.stored       = phys;
            }
            return;
        }

        s.stored      = phys;
        const float d = phys - s.anchor;
        if (d > kPotStillBand || d < -kPotStillBand)
        {
            s.anchor       = phys;
            s.last_move_ms = t_ms;
        }
        else if (t_ms - s.last_move_ms > kPotSleepMs)  /* wrap-safe */
        {
            s.awake = false;
        }
        return;
    }

    const float diff = phys - s.stored;
    if (diff > -kCatchTolerance && diff < kCatchTolerance)
    {
        s.caught = true;
        s.stored = phys;
    }
    else
    {
        const int cur_sign = (diff > 0.0f) ? 1 : -1;
        if (s.sign_at_lock == 0 || cur_sign == s.sign_at_lock) return;
        s.caught = true;
        s.stored = phys;
    }
    /* A fresh catch means the user is on the pot. */
    s.awake        = true;
    s.anchor       = phys;
    s.last_move_ms = t_ms;
}

} // namespace alchemy
