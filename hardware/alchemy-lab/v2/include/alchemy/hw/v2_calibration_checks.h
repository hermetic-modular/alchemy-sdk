#pragma once

#include <cmath>
#include "alchemy/hw/v2_calibration.h"

namespace alchemy {

// Broad manufacturing guardrails, not a claim of absolute metrology accuracy.
// Keep identical across the boot calibration and the production tester.
constexpr float kV2CalMaxNoiseV = 0.025f;
constexpr float kV2CalMaxPeakToPeakV = 0.250f;
constexpr float kV2CalMaxResidualV = 0.100f;
constexpr float kV2CalCrossErrorV = 0.150f;
// Full-code endpoints include DAC buffer headroom and supply variation.
// Require useful +/-4 V coverage; the interior fit and fresh +/-3.7 V
// verification independently reject faulty or overly compressed channels.
constexpr float kV2CalMinEndpointV = 4.0f;
constexpr float kV2CalMaxEndpointV = 6.0f;

struct V2CalSamples
{
    uint32_t count = 0;
    double mean = 0, m2 = 0;
    uint16_t minimum = 65535, maximum = 0;
    void Add(uint16_t value)
    {
        ++count;
        const double delta = value - mean;
        mean += delta / count;
        m2 += delta * (value - mean);
        if (value < minimum) minimum = value;
        if (value > maximum) maximum = value;
    }
    double StdDev() const { return count > 1 ? std::sqrt(m2 / (count - 1)) : 0; }
};

inline bool V2CalVddaValid(float v)
{
    return std::isfinite(v) && v >= 3.0f && v <= 3.6f;
}

inline bool V2CalZeroValid(double raw, float vdda)
{
    const double pin_v = raw * vdda / 65535.0;
    return V2CalVddaValid(vdda) && std::isfinite(raw)
        && pin_v >= 1.35 && pin_v <= 1.95;
}

inline bool V2CalSamplesValid(const V2CalSamples& s, float vdda)
{
    const double scale = vdda / (65535.0 * kV2CvInGainDesign);
    return s.count >= 32 && V2CalVddaValid(vdda)
        && std::isfinite(s.mean) && std::isfinite(s.m2) && s.m2 >= 0
        && s.minimum > 64 && s.maximum < 65471
        && s.mean >= s.minimum && s.mean <= s.maximum
        && s.StdDev() * scale <= kV2CalMaxNoiseV
        && (s.maximum - s.minimum) * scale <= kV2CalMaxPeakToPeakV;
}

inline bool V2CalJackValid(const V2JackCal& j, float vdda)
{
    const float slope = j.dac_gain_v_per_code;
    const float offset = j.dac_offset_v;
    const float nominal = kV2CvOutGainDesign * kV2VddaDesign / 4096.0f;
    return V2CalZeroValid(j.adc_zero_code, vdda)
        && std::isfinite(slope) && std::isfinite(offset)
        && slope >= nominal * 1.20f && slope <= nominal * 0.80f
        && offset >= 4.0f && offset <= 6.0f
        && j.dac_min_linear_code <= 1024
        && j.dac_max_linear_code >= 3071 && j.dac_max_linear_code <= 4095
        && j.dac_max_linear_code > j.dac_min_linear_code
        && j.dac_max_linear_code - j.dac_min_linear_code >= 2800
        && offset + slope * j.dac_min_linear_code >= 3.5f
        && offset + slope * j.dac_max_linear_code <= -3.5f;
}

inline bool V2CalRecordPlausible(const V2Calibration& c)
{
    if (!V2CalVddaValid(c.vdda_at_cal) || c.vdda_source > kV2VddaAssumed)
        return false;
    for (const auto& j : c.jack)
        if (!V2CalJackValid(j, c.vdda_at_cal)) return false;
    return true;
}

} // namespace alchemy
