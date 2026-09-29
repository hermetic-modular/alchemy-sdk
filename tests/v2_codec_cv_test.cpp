/** Host regression tests for the J9/J10 fixed voltage transfer. */
#include "alchemy/hw/v2_codec_cv.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {
int failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        std::printf("FAIL line %d: %s\n", __LINE__, #expr); \
        ++failures; \
    } \
} while (0)

bool Near(float actual, float expected, float tolerance = 1e-6f)
{
    return std::fabs(actual - expected) <= tolerance;
}

/* Independent forward model: each DAC leg swings +/-2 V around the
 * common mode. Figure 1.4 subtracts those legs with a 39k/18k gain. */
float NominalJackVolts(float sample)
{
    const float plus  = 2.5f + 2.0f * sample;
    const float minus = 2.5f - 2.0f * sample;
    return (minus - plus) * (39000.0f / 18000.0f);
}
} // namespace

int main()
{
    using alchemy::V2CodecCvSample;

    // Issue #48: +5 V must not be encoded as a positive full-scale sample.
    CHECK(Near(V2CodecCvSample(5.0f), -0.576923077f));
    CHECK(Near(V2CodecCvSample(-5.0f), 0.576923077f));
    CHECK(V2CodecCvSample(0.0f) == 0.0f);

    const float targets[] = {-5.0f, -2.5f, -1.0f, 0.0f, 1.0f, 2.5f, 5.0f};
    for (float volts : targets)
    {
        const float sample = V2CodecCvSample(volts);
        CHECK(std::isfinite(sample));
        CHECK(sample >= -1.0f && sample <= 1.0f);
        CHECK(Near(NominalJackVolts(sample), volts));
    }

    // Keep finite out-of-range commands bounded at the digital rails.
    CHECK(V2CodecCvSample(9.0f) == -1.0f);
    CHECK(V2CodecCvSample(-9.0f) == 1.0f);
    CHECK(V2CodecCvSample(std::numeric_limits<float>::max()) == -1.0f);
    CHECK(V2CodecCvSample(std::numeric_limits<float>::lowest()) == 1.0f);
    CHECK(Near(NominalJackVolts(-1.0f), 8.666666667f));
    CHECK(Near(NominalJackVolts(1.0f), -8.666666667f));

    // Saturation must start at the physical rail, not the +/-5 V API range.
    CHECK(Near(NominalJackVolts(V2CodecCvSample(8.0f)), 8.0f));
    CHECK(Near(NominalJackVolts(V2CodecCvSample(-8.0f)), -8.0f));

    // Defense at the audio boundary: no NaN/Inf reaches libDaisy conversion.
    CHECK(V2CodecCvSample(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
    CHECK(V2CodecCvSample(std::numeric_limits<float>::infinity()) == 0.0f);
    CHECK(V2CodecCvSample(-std::numeric_limits<float>::infinity()) == 0.0f);

    if (failures)
    {
        std::printf("v2_codec_cv_test: %d failures\n", failures);
        return 1;
    }
    std::puts("v2_codec_cv_test: all tests passed");
    return 0;
}
