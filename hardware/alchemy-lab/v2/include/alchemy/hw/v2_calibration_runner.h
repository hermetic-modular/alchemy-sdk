#pragma once

#include "alchemy/hw/v2_calibration_checks.h"

namespace alchemy {

enum class V2CalFailure : uint8_t {
    None, Io, Reference, Zero, Noise, Response, Fit, Range, Isolation,
    Verification, Cancelled
};

struct V2CalJackReport {
    // 0 not started, 1 zero, 2 isolation, 3 sweep, 4 fit, 5 verify, 6 complete.
    uint8_t stage = 0, sweep_points = 0, verification_points = 0;
    float zero_raw = 0;
    float span_v = 0;
    float noise_v = 0;
    float residual_v = 0;
    float verification_v = 0;
    float cross_error_v = 0;
    float sweep_v[17] = {};
};

struct V2CalReport {
    V2CalFailure failure = V2CalFailure::None;
    uint8_t jack = 255;
    uint8_t step = 0;
    // A failing measurement is retained even when no usable candidate exists.
    // metric: 1 reference, 2 zero, 3 noise, 4 peak-to-peak, 5 ADC saturation,
    // 6 disconnected output, 7 paired input, 8 unintended channel,
    // 9 interior step, 10 reversed step, 11 positive endpoint,
    // 12 negative endpoint, 13 fit residual, 14 coefficients, 15 verification.
    uint8_t metric = 0, measured_jack = 255;
    float observed = 0, low = 0, high = 0;
    V2CalJackReport channels[6];
};

// Adapters own ADC lifetime, timing and cancellation. Capture must acquire
// fresh conversions; Wait/Capture must remain bounded and service the host.
class V2CalIo {
  public:
    virtual ~V2CalIo() = default;
    virtual bool Reference(float& vdda) = 0;
    virtual bool AllOff() = 0;
    virtual bool Write(uint8_t jack, uint16_t code) = 0;
    virtual bool Connect(uint8_t jack) = 0;
    virtual bool Capture(uint8_t jack, uint32_t count, V2CalSamples& out) = 0;
    virtual bool Wait(uint32_t ms) = 0;
    virtual bool Cancelled() const { return false; }
    virtual void Progress(uint8_t jack, uint8_t step) {}
};

// Does not persist. On any failure performs best-effort disconnection and
// leaves the candidate unusable (magic/CRC zero). paired=true is exclusively
// for the fixed J3-J8, J4-J7, J5-J6 harness. Boot calibration uses false.
bool V2RunCalibration(V2CalIo& io, bool paired, V2Calibration& candidate,
                      V2CalReport& report);

} // namespace alchemy
