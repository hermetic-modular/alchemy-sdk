/**
 * @file alchemy/control/lock_snap.h
 * @brief Pure snap math for Clocked parameter locks.
 *
 * Given a recorded duration in musical-clock ticks, pick the grid length
 * it "meant": the candidate minimizing the ratio distance max(T/c, c/T).
 * Ratio distance (equivalently, nearest in the log domain) is the right
 * metric because the correction is the trimmed tail or held gap at the
 * loop seam, and it is its size *relative to the loop* that you hear —
 * being 20 ms long matters a lot at a 16th and not at all at 4 bars.
 *
 * Candidates are the enabled note-value families up to a whole note, plus
 * integer bar counts (always enabled) from 1 bar up to just past the
 * recording.  Integer bars keep the worst-case correction small above one
 * bar, where the pure note-value grid goes sparse (a 5½-bar take snaps to
 * 5 or 6 bars, not 4 or 8).
 *
 * Stateless and host-testable; consumed by ParamLockManager at slot
 * activation.
 */

#pragma once

#include <cstdint>
#include "alchemy/control/lock_types.h"

namespace alchemy {

/** Largest representable snapped length (u16 ticks in the saved form). */
constexpr uint32_t kLockSnapMaxTicks = 65535u;

/**
 * Snap a recorded duration to the musical grid.
 *
 * @param rec_ticks      Recorded duration in clock ticks (> 0).
 * @param ppqn           Clock resolution (multiple of 24; see MusicalClock).
 * @param beats_per_bar  Time-signature numerator.
 * @param grid_mask      LockGrid family bitmask (bars are always included).
 * @return               Snapped length in ticks, in
 *                       [smallest enabled candidate, kLockSnapMaxTicks];
 *                       0 — the caller treats the slot as free — if the
 *                       configuration is degenerate or the take is too
 *                       long to represent (better free than trimmed).
 */
inline uint32_t LockSnapTicks(double  rec_ticks,
                              uint32_t ppqn,
                              uint8_t  beats_per_bar,
                              uint8_t  grid_mask)
{
    if (!(rec_ticks > 0.0) || ppqn == 0u || beats_per_bar == 0u
        || rec_ticks > static_cast<double>(kLockSnapMaxTicks))
        return 0u;

    uint32_t best   = 0u;
    double   best_r = 0.0;

    const auto consider = [&](uint32_t num, uint32_t den)
    {
        if (den == 0u || num % den != 0u) return;   /* inexact at this ppqn */
        const uint32_t c = num / den;
        if (c == 0u || c > kLockSnapMaxTicks) return;
        const double cd = static_cast<double>(c);
        const double r  = (rec_ticks > cd) ? rec_ticks / cd : cd / rec_ticks;
        if (best == 0u || r < best_r)
        {
            best   = c;
            best_r = r;
        }
    };

    if (grid_mask & LockGrid::Straight)
    {
        consider(ppqn, 4u);        /* 16th    */
        consider(ppqn, 2u);        /* 8th     */
        consider(ppqn, 1u);        /* quarter */
        consider(ppqn * 2u, 1u);   /* half    */
        consider(ppqn * 4u, 1u);   /* whole   */
    }
    if (grid_mask & LockGrid::Triplet)
    {
        consider(ppqn, 6u);        /* 16th-t    */
        consider(ppqn, 3u);        /* 8th-t     */
        consider(ppqn * 2u, 3u);   /* quarter-t */
        consider(ppqn * 4u, 3u);   /* half-t    */
        consider(ppqn * 8u, 3u);   /* whole-t   */
    }
    if (grid_mask & LockGrid::Dotted)
    {
        consider(ppqn * 3u, 8u);   /* dotted 16th    */
        consider(ppqn * 3u, 4u);   /* dotted 8th     */
        consider(ppqn * 3u, 2u);   /* dotted quarter */
        consider(ppqn * 3u, 1u);   /* dotted half    */
        consider(ppqn * 6u, 1u);   /* dotted whole   */
    }

    /* Integer bars, always: 1 bar up to the first count past the
     * recording (so both neighbours of T are candidates), within u16. */
    const uint32_t bar = ppqn * static_cast<uint32_t>(beats_per_bar);
    for (uint32_t k = 1u; ; k++)
    {
        const uint64_t c = static_cast<uint64_t>(bar) * k;
        if (c > kLockSnapMaxTicks) break;
        consider(static_cast<uint32_t>(c), 1u);
        if (static_cast<double>(c) >= rec_ticks) break;
    }

    return best;
}

} // namespace alchemy
