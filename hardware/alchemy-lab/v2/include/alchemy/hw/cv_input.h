/**
 * @file cv_input.h
 * @brief alchemy::CvInput — Alchemy Lab V2 CV jack wrapper.
 *
 * Volts() has two paths:
 *   - CALIBRATED (preferred): uses this channel's measured idle code and
 *     the board's measured VDDA from the V2Calibration record. Applied
 *     automatically by AlchemyLabV2::Init() when a valid record exists.
 *   - Design fallback: assumes ideal bias (mid-scale) and VDDA = 3.30 V.
 *     Accurate to hardware tolerance only (~3-5 %).
 *
 * `Value()` stays the raw normalised 0..1 control reading (flip applied:
 * higher Value = more positive jack voltage); pots-style consumers keep
 * using it unchanged.
 */

#pragma once

#include "daisy_seed.h"

#include "alchemy/hw/v2_calibration.h"

namespace alchemy {

class CvInput
{
  public:
    void Init(uint16_t* adcptr, float sample_rate, bool flip, bool invert, float slew)
    {
        ac_.Init(adcptr, sample_rate, flip, invert, slew);
    }

    /** Apply per-channel calibration: zero code from the idle pass and
     *  the board VDDA, both out of the V2Calibration record. Reached via
     *  CvJack::Init* during AlchemyLabV2::Init(); harmless to call with
     *  design-fallback values (32768 / 3.30) — that reproduces the
     *  uncalibrated transfer exactly. */
    void SetCalibration(uint16_t adc_zero_code, float vdda)
    {
        /* With flip=true, Value() = 1 − raw/65535. Jack volts:
         *   v = (zero_raw − raw) × vdda / 65535 / kV2CvInGainDesign
         *     = (Value() − (1 − zero_raw/65535)) × vdda / kV2CvInGainDesign */
        cal_offset_ = 1.0f - static_cast<float>(adc_zero_code) / 65535.0f;
        cal_scale_  = vdda / kV2CvInGainDesign;
        calibrated_ = true;
    }

    void Process() { ac_.Process(); }

    /** Raw normalised reading, 0..1 (flip applied — higher = more
     *  positive jack voltage). Full span corresponds to ±10 V at the
     *  jack; 0 V sits near 0.5. */
    float Value() const { return ac_.Value(); }

    /** Signed jack voltage. Calibrated when the board has a valid
     *  V2Calibration record; design-nominal otherwise (VDDA = 3.30,
     *  ideal mid-scale bias → (Value() − 0.5) × 20). */
    float Volts() const
    {
        if (calibrated_)
            return (ac_.Value() - cal_offset_) * cal_scale_;
        return (ac_.Value() - 0.5f) * kDesignScale;
    }

  private:
    /* Design-nominal jack scale, VDDA/gain = 3.30/0.165 → 20 V per unit
     * of Value(). Same constants the factory-cal fit is derived from. */
    static constexpr float kDesignScale = kV2VddaDesign / kV2CvInGainDesign;

    daisy::AnalogControl ac_;
    float cal_offset_ = 0.5f;
    float cal_scale_  = kDesignScale;
    bool  calibrated_ = false;
};

} // namespace alchemy
