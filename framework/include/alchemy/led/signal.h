/**
 * @file alchemy/led/signal.h
 * @brief Signal helpers for ring animation — phase clocks, tapers, hashes.
 *
 * Every animated layer in the ring composition kit is driven by plain
 * floats.  These helpers produce the common ones:
 *
 *   Phase01      — sawtooth clock phase from wall time.
 *   Tri01        — triangle 0→1→0 from a phase (ping-pong mapping).
 *   Hash01       — deterministic per-(unit, bucket) value in [0, 1).
 *                  Frame-coherent texture, not randomness: the same
 *                  inputs always produce the same value.
 *   NormTapered  — display mapping with a floor, a ceiling, and a curve.
 *                  Use when a raw signal spans several orders of
 *                  magnitude (delay lag, decay time) and a linear map
 *                  would pin the indicator at one end.
 *
 * All functions are pure and allocation-free.
 */

#pragma once

#include <cstdint>
#include <cmath>

namespace alchemy {

/** Sawtooth phase 0..1 with the given period.  period_ms == 0 returns 0. */
inline float Phase01(uint32_t t_ms, uint32_t period_ms)
{
    if (period_ms == 0u) return 0.0f;
    return static_cast<float>(t_ms % period_ms)
         / static_cast<float>(period_ms);
}

/** Triangle 0→1→0 over one cycle of @p phase.  Accepts any non-negative
 *  phase; fractional part is used. */
inline float Tri01(float phase)
{
    const float x = phase - static_cast<float>(static_cast<int>(phase));
    return x < 0.5f ? (2.0f * x) : (2.0f - 2.0f * x);
}

/** Deterministic hash of (unit, bucket) in [0, 1).  Integer mix — cheap,
 *  identical across boots and frame rates. */
inline float Hash01(uint32_t unit, uint32_t bucket)
{
    uint32_t h = (unit * 2654435761u) ^ (bucket * 2246822519u);
    h ^= h >> 15;
    h *= 2654435761u;
    h ^= h >> 13;
    return static_cast<float>(h & 0xFFFFu) * (1.0f / 65536.0f);
}

/** Curve selector for NormTapered. */
enum class Taper : uint8_t
{
    Linear,  ///< Proportional across the range.
    Sqrt,    ///< Near-proportional at the low end, compressed at the top.
    Log,     ///< Equal arc per decade.  Steep near the floor — prefer
             ///< Sqrt when the signal spends time near zero.
};

/**
 * Map @p x onto [0, 1] for display: below @p floor returns a negative
 * value (caller hides the indicator), at/above @p ceiling returns 1
 * (pegged), in between applies the taper.
 *
 * Requires 0 < floor < ceiling.
 */
inline float NormTapered(float x, float floor_v, float ceiling_v,
                         Taper curve = Taper::Sqrt)
{
    if (x <= floor_v || ceiling_v <= floor_v) return -1.0f;
    if (x >= ceiling_v) return 1.0f;

    switch (curve)
    {
        case Taper::Sqrt:
            return std::sqrt(x / ceiling_v);
        case Taper::Log:
            return std::log(x / floor_v) / std::log(ceiling_v / floor_v);
        case Taper::Linear:
        default:
            return (x - floor_v) / (ceiling_v - floor_v);
    }
}

} // namespace alchemy
