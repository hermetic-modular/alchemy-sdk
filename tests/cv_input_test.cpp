/**
 * @file cv_input_test.cpp
 * @brief Host tests for CvInput's Volts() transfer — design fallback and
 *        the calibrated path (per-jack zero code + board VDDA).
 * Drives the stub daisy::AnalogControl, which mirrors the real flip
 * semantics: Value() = 1 − raw/65535.
 */

#include "alchemy/hw/cv_input.h"
#include "alchemy/hw/v2_calibration.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

constexpr float kCtrlRate = 3000.f;  /* control rate; irrelevant to math */

/* One CvInput wired to a settable raw ADC code, flip=true like CvJack. */
struct Rig
{
    uint16_t         raw = 32768u;
    alchemy::CvInput in;

    Rig()
    {
        in.Init(&raw, kCtrlRate, /*flip=*/true, /*invert=*/false,
                /*slew=*/0.0f);
    }

    float VoltsAt(uint16_t code)
    {
        raw = code;
        in.Process();
        return in.Volts();
    }
};

/* Forward model of the V2 input chain (inverting summer into the ADC):
 *   V_pin = bias − kV2CvInGainDesign × V_jack,  raw = V_pin/vdda × 65535 */
uint16_t RawFor(float v_jack, float vdda, float bias_v)
{
    float v_pin = bias_v - alchemy::kV2CvInGainDesign * v_jack;
    float n     = v_pin / vdda;
    if (n < 0.f) n = 0.f;
    if (n > 1.f) n = 1.f;
    return static_cast<uint16_t>(n * 65535.f + 0.5f);
}

bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

/* Measured V-per-Value slope over an exact half-span: raw 16384 → 49152
 * is ΔValue = 32768/65535; offsets cancel, leaving cal_scale_. */
float MeasuredScale(Rig& r)
{
    const float hi = r.VoltsAt(16384u);  /* flip: lower raw = higher V */
    const float lo = r.VoltsAt(49152u);
    return (hi - lo) * 65535.f / 32768.f;
}

} // namespace

int main()
{
    /* 1) Design fallback: full span ±10 V, 0 V at mid-scale, slope 20. */
    {
        Rig r;
        assert(Near(r.VoltsAt(0u), 10.f, 1e-4f));
        assert(Near(r.VoltsAt(65535u), -10.f, 1e-4f));
        assert(Near(r.VoltsAt(32768u), 0.f, 1e-3f));
        assert(Near(MeasuredScale(r), 20.f, 1e-3f));
    }

    /* 2) Calibrated slope is vdda/gain — NOT the design 20.0. This is
     *    the regression pin: a hardcoded 3.30 V makes this 20.0 on
     *    every board regardless of the record. */
    {
        Rig r;
        r.in.SetCalibration(32768u, 3.20f);
        const float scale = MeasuredScale(r);
        assert(Near(scale, 3.20f / alchemy::kV2CvInGainDesign, 1e-3f));
        assert(!Near(scale, 20.0f, 0.1f));
    }

    /* 3) The stored zero code maps to exactly 0 V, off-mid-scale too. */
    {
        Rig r;
        r.in.SetCalibration(33500u, 3.26f);
        assert(Near(r.VoltsAt(33500u), 0.f, 1e-3f));
    }

    /* 4) Round trip through the physical model at off-nominal VDDA and
     *    bias: calibrating with that board's zero code + VDDA recovers
     *    jack volts to well under an ADC LSB (~0.3 mV). */
    {
        const float vdda = 3.26f, bias = 1.66f;
        Rig r;
        r.in.SetCalibration(RawFor(0.f, vdda, bias), vdda);
        const float probes[] = {-5.f, -2.f, -0.5f, 0.f, 0.75f, 3.f, 4.8f};
        for (float v : probes)
            assert(Near(r.VoltsAt(RawFor(v, vdda, bias)), v, 2e-3f));
    }

    /* 5) The field report that surfaced the bug: a board that tracks
     *    1 V/oct at scale 19.75 ⇔ VDDA ≈ 3.259 V in its cal record. */
    {
        Rig r;
        r.in.SetCalibration(32768u, 19.75f * alchemy::kV2CvInGainDesign);
        assert(Near(MeasuredScale(r), 19.75f, 1e-3f));
    }

    std::printf("cv_input_test: all tests passed\n");
    return 0;
}
