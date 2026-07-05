/**
 * @file field.h
 * @brief Field: a region-wide brightness modulation for ring composition.
 *
 * A Field is not an element — it is a texture in the brightness of LEDs
 * that are already lit.  Each frame it computes a modulation m(unit, t)
 * in [0, 1] and applies gain = 1 − amount·m across a region of a
 * RingFrame.  Fields attenuate; they never recolor.
 *
 * The modulation is the product of three independent choices:
 *
 *   pattern — the shape across space:
 *     Uniform — the same level everywhere (breathing, blinking).
 *     Noise   — a hash per spatial unit (shimmer, fuzz, stutter).
 *     Wave    — a triangle wave across the region that drifts over time
 *               (ripple, climb).
 *
 *   grain — the spatial unit:
 *     Whole   — one unit for the whole region.
 *     PerLed  — every LED its own unit.
 *     Chunk   — bands of chunk_leds LEDs share a unit.
 *
 *   step — the temporal behavior:
 *     Smooth  — continuous motion.
 *     Held    — sample-and-hold: the value jumps once per rate_ms bucket.
 *
 * The familiar effects are parameter combinations, provided as presets:
 *
 *   FieldPulse()     Uniform · Whole  · Smooth   slow breath
 *   FieldRipple()    Wave    · PerLed · Smooth   drifting wave
 *   FieldShimmer()   Noise   · PerLed · Held     per-LED twinkle
 *   FieldStutter()   Noise   · Whole  · Held     whole-region S&H jumps
 *   FieldStaircase() Wave    · Chunk  · Smooth   static banding (rate 0)
 *
 * Time-driven motion uses t_ms and rate_ms.  Signal-driven motion passes
 * an explicit phase (0..1) instead — e.g. a DSP-published LFO or Shepard
 * phase — and the field moves at the audio's true rate.
 */

#pragma once

#include <cstdint>

namespace alchemy {

enum class FieldPattern : uint8_t
{
    Uniform,  ///< Same level across the region.
    Noise,    ///< Hash per spatial unit.
    Wave,     ///< Triangle wave across the region, drifting over time.
};

enum class FieldGrain : uint8_t
{
    Whole,    ///< One unit for the whole region.
    PerLed,   ///< Each LED is its own unit.
    Chunk,    ///< Bands of chunk_leds LEDs share a unit.
};

enum class FieldStep : uint8_t
{
    Smooth,   ///< Continuous motion.
    Held,     ///< Sample-and-hold per rate_ms bucket.
};

struct FieldDesc
{
    FieldPattern pattern      = FieldPattern::Uniform;
    FieldGrain   grain        = FieldGrain::Whole;
    FieldStep    step         = FieldStep::Smooth;
    uint16_t     rate_ms      = 1000u;  ///< Period (Smooth) or hold bucket (Held).
    uint8_t      chunk_leds   = 2u;     ///< Band width (Chunk grain).
    float        spatial_leds = 4.0f;   ///< Wave wavelength in LEDs; sign sets drift direction.
    bool         invert       = false;  ///< m → 1−m: lift from a floor instead of dipping.
};

/**
 * Evaluate the modulation for one spatial unit.
 *
 * @param unit     Spatial unit index (0 for Whole grain).
 * @param t_ms     Frame time; drives motion when @p phase01 < 0.
 * @param phase01  Explicit phase 0..1, or negative to derive from t_ms
 *                 and rate_ms.
 * @return         m in [0, 1], before amount scaling and inversion.
 */
float FieldEval(const FieldDesc& desc, uint32_t unit,
                uint32_t t_ms, float phase01);

/* ── Presets — parameter combinations, not types ────────────────────────── */

inline FieldDesc FieldPulse(uint16_t period_ms = 2000u)
{
    FieldDesc d;
    d.pattern = FieldPattern::Uniform;
    d.rate_ms = period_ms;
    return d;
}

inline FieldDesc FieldRipple(uint16_t period_ms   = 2000u,
                             float    spatial_leds = 8.0f)
{
    FieldDesc d;
    d.pattern      = FieldPattern::Wave;
    d.grain        = FieldGrain::PerLed;
    d.rate_ms      = period_ms;
    d.spatial_leds = spatial_leds;
    return d;
}

inline FieldDesc FieldShimmer(uint16_t bucket_ms = 70u)
{
    FieldDesc d;
    d.pattern = FieldPattern::Noise;
    d.grain   = FieldGrain::PerLed;
    d.step    = FieldStep::Held;
    d.rate_ms = bucket_ms;
    return d;
}

inline FieldDesc FieldStutter(uint16_t hold_ms = 120u)
{
    FieldDesc d;
    d.pattern = FieldPattern::Noise;
    d.grain   = FieldGrain::Whole;
    d.step    = FieldStep::Held;
    d.rate_ms = hold_ms;
    return d;
}

inline FieldDesc FieldStaircase(uint8_t chunk_leds = 2u)
{
    FieldDesc d;
    d.pattern      = FieldPattern::Wave;
    d.grain        = FieldGrain::Chunk;
    d.chunk_leds   = chunk_leds;
    d.rate_ms      = 0u;                 /* static */
    d.spatial_leds = 2.0f * chunk_leds;  /* alternate bands */
    return d;
}

} // namespace alchemy
