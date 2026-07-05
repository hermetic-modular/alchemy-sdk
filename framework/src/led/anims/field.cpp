/**
 * @file field.cpp
 */

#include "alchemy/led/anims/field.h"
#include "alchemy/led/signal.h"

#include <cmath>

namespace alchemy {

namespace {

constexpr float kTwoPi = 6.28318530f;

inline float Frac(float x)
{
    return x - std::floor(x);
}

} // namespace

float FieldEval(const FieldDesc& desc, uint32_t unit,
                uint32_t t_ms, float phase01)
{
    switch (desc.pattern)
    {
        case FieldPattern::Uniform:
        {
            if (desc.step == FieldStep::Held)
            {
                /* Uniform means one level across the region regardless
                 * of grain, so the hash ignores the unit. */
                const uint32_t bucket =
                    desc.rate_ms ? (t_ms / desc.rate_ms) : 0u;
                return Hash01(0u, bucket);
            }
            const float p = phase01 >= 0.0f
                                ? phase01
                                : Phase01(t_ms, desc.rate_ms);
            /* Raised cosine: m = 0 at phase 0, so a fill breathes from
             * its drawn brightness rather than starting mid-dip. */
            return 0.5f * (1.0f - std::cos(kTwoPi * p));
        }

        case FieldPattern::Noise:
        {
            const uint32_t bucket =
                desc.rate_ms ? (t_ms / desc.rate_ms) : 0u;
            if (desc.step == FieldStep::Held)
                return Hash01(unit, bucket);
            /* Smooth noise: crossfade between consecutive buckets. */
            const float fb =
                desc.rate_ms
                    ? static_cast<float>(t_ms % desc.rate_ms)
                          / static_cast<float>(desc.rate_ms)
                    : 0.0f;
            const float a = Hash01(unit, bucket);
            const float b = Hash01(unit, bucket + 1u);
            return a + (b - a) * fb;
        }

        case FieldPattern::Wave:
        {
            if (desc.spatial_leds == 0.0f) return 0.0f;
            float p = phase01 >= 0.0f ? phase01
                                      : Phase01(t_ms, desc.rate_ms);
            if (desc.step == FieldStep::Held)
                p = std::floor(p * 8.0f) * 0.125f;
            /* +64 keeps the argument positive for any plausible unit
             * count and wavelength sign. */
            return Tri01(Frac(64.0f
                              + static_cast<float>(unit) / desc.spatial_leds
                              - p));
        }
    }
    return 0.0f;
}

} // namespace alchemy
