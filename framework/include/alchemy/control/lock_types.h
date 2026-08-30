/**
 * @file alchemy/control/lock_types.h
 * @brief Behavior vocabulary for parameter locks: sync mode, exit policy,
 *        snap-grid families, LED style animations.
 *
 * Capacity policy (how much motion fits) lives in lock_length.h; this
 * header is how a lock *behaves*.  Everything here is a small closed set
 * consumed by the ParamLock surface's fluent configuration calls — see
 * docs/param-locks.md.
 */

#pragma once

#include <cstdint>

namespace alchemy {

/* ── Gesture thresholds ───────────────────────────────────────────────── */

/** Physical nudge (0..1) that arms recording on an empty slot.  Small on
 *  purpose — the distance to arm is timing error on the loop's t=0.
 *  Hardware-verified to sit ~3× above the filtered pot noise floor (the
 *  baseline snapshots at button-down, so noise cannot creep toward it). */
constexpr float kParamLockArmThreshold   = 0.005f;

/** Physical nudge (0..1) that clears an active slot.  Deliberately larger
 *  than the arm threshold: brushing a neighboring pot mid-hold must not
 *  destroy a playing lock. */
constexpr float kParamLockClearThreshold = 0.03f;

/* ── Sync mode ────────────────────────────────────────────────────────── */

/**
 * Whether new recordings snap their loop length to the musical clock.
 *
 * Applies at record time only: each slot keeps the truth it was recorded
 * with (and serializes it), so flipping the mode never re-times an
 * existing lock.
 */
enum class LockSync : uint8_t
{
    Free    = 0,   ///< loop replays at the wall-clock length it was recorded
    Clocked = 1,   ///< loop length snaps to the nearest musical division
};

/* ── Exit policy ──────────────────────────────────────────────────────── */

/**
 * What happens to the knob's base value when recording ends.
 *
 *   Latch  — the pot stays where the gesture left it; playback rides on
 *            top as an offset the user can keep performing.
 *   Return — the logical value snaps back to where the gesture started
 *            (record_base) and pot-catch re-arms, so playback reproduces
 *            exactly what was recorded until the pot is caught again.
 *            Requires the paged ParamLock form (a Pager to write into).
 */
enum class LockExit : uint8_t
{
    Latch  = 0,
    Return = 1,
};

/* ── Snap grid ────────────────────────────────────────────────────────── */

/**
 * Bitmask of note-value families a Clocked recording may snap to.
 * Integer bar counts above one bar are always candidates regardless of
 * the mask — without them a 5½-bar take would stretch to 4 or 8 bars.
 */
namespace LockGrid {

constexpr uint8_t Straight = 1u << 0;  ///< 16th, 8th, quarter, half, whole
constexpr uint8_t Triplet  = 1u << 1;  ///< triplet variants of the above
constexpr uint8_t Dotted   = 1u << 2;  ///< dotted 16th … dotted whole

constexpr uint8_t Default  = Straight | Triplet;

} // namespace LockGrid

/* ── LED styles ───────────────────────────────────────────────────────── */

/**
 * Pip animation for the recording / playback overlay.  A small curated
 * set with no tuning knobs; full control remains the existing escape
 * hatch (skip the stock Render() and compose your own ring from
 * IsActive / IsRecording / PlayPhase).
 */
enum class LockAnim : uint8_t
{
    Solid,      ///< steady pip
    Blink,      ///< 2 Hz square blink
    Breathe,    ///< slow sinusoidal breath (~2 s)
    LoopPulse,  ///< flash at each loop restart, decaying over its first
                ///< fifth.  Solid while recording (no loop exists yet).
    Sweep,      ///< pip orbits the ring with the play head; while
                ///< recording it crawls with the write head (buffer use)
};

/**
 * One evaluated animation frame: a brightness factor for the pip and,
 * for Sweep, where on the ring to draw it.
 *
 * Pure function of its inputs (stateless, frame-rate independent —
 * docs/ring-animations.md rule 3).
 *
 * @param anim       Style to evaluate.
 * @param t_ms       Absolute milliseconds (drives Blink / Breathe).
 * @param phase      Loop phase 0..1: play head for playback, write
 *                   position / capacity while recording.
 * @param recording  True while the slot is recording.
 */
struct LockAnimFrame
{
    float level;      ///< brightness multiplier 0..1
    bool  sweep;      ///< true → draw at sweep_pos01 instead of the home pip
    float sweep_pos01;///< ring position 0..1 (0 = home / 6 o'clock)
};

inline LockAnimFrame LockAnimEval(LockAnim anim, uint32_t t_ms,
                                  float phase, bool recording)
{
    if (phase < 0.0f) phase = 0.0f;
    if (phase > 1.0f) phase = 1.0f;

    switch (anim)
    {
        case LockAnim::Blink:
            return {(t_ms % 500u) < 250u ? 1.0f : 0.0f, false, 0.0f};

        case LockAnim::Breathe:
        {
            /* 2 s triangle — a cosine reads identically on a single pip. */
            const uint32_t t   = t_ms % 2000u;
            const float    lin = (t < 1000u)
                                   ? static_cast<float>(t) / 1000.0f
                                   : static_cast<float>(2000u - t) / 1000.0f;
            return {0.25f + 0.75f * lin, false, 0.0f};
        }

        case LockAnim::LoopPulse:
        {
            if (recording) return {1.0f, false, 0.0f};
            const float decay = 1.0f - phase * 5.0f;
            const float k     = decay > 0.0f ? decay : 0.0f;
            return {0.2f + 0.8f * k, false, 0.0f};
        }

        case LockAnim::Sweep:
            return {1.0f, true, phase};

        case LockAnim::Solid:
        default:
            return {1.0f, false, 0.0f};
    }
}

} // namespace alchemy
