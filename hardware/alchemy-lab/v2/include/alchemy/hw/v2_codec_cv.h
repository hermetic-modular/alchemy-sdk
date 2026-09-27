/**
 * @file v2_codec_cv.h
 * @brief Fixed J9/J10 voltage conversion for the Seed2 DFM output circuit.
 */
#pragma once

#include <cmath>

namespace alchemy {

/* Seed2 DFM datasheet, Figure 1.4 (Eurorack DC-coupled output):
 * https://daisy.nyc3.cdn.digitaloceanspaces.com/products/seed2-dfm/Daisy_Seed2_DFM_v1-0-12.pdf
 * The differential receiver has DC gain -39k/18k. The PCM3060 supplies
 * 8 Vpp differential at full scale with its nominal 5 V supply:
 * https://www.ti.com/lit/ds/symlink/pcm3060.pdf
 * Thus a +1 sample produces nominally -8.667 V at the jack. This assumes
 * the stock Seed audio gain; it is not a per-board calibration.
 * Keep this physical transfer separate from CvJack::Value()'s +/-5 V
 * normalized target range.
 */
constexpr float kV2CodecCvVoltsPerSample = -4.0f * (39.0f / 18.0f);

/** Convert jack volts to a bounded codec sample. Invalid input is silent;
 *  CvJack::SetVolts rejects it before changing the staged target. */
inline float V2CodecCvSample(float volts)
{
    if (!std::isfinite(volts)) return 0.0f;

    const float sample = volts / kV2CodecCvVoltsPerSample;
    if (sample < -1.0f) return -1.0f;
    if (sample >  1.0f) return  1.0f;
    return sample;
}

} // namespace alchemy
