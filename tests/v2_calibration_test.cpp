#include "alchemy/hw/v2_calibration_runner.h"
#include <cassert>
#include <cstdio>
#include <limits>
#include <initializer_list>

using namespace alchemy;

struct Rig : V2CalIo {
    enum Fault { None, Dead, Reversed, Noisy, Bias, Clip, Nonlinear, SwitchClosed,
                 SwitchOpen, Bridge, BadReference, IoError, Cancel, VerifyDrift,
                 BufferHeadroom } fault = None;
    bool paired = false;
    bool connected[6] = {};
    uint16_t codes[6] = {2048,2048,2048,2048,2048,2048};
    uint32_t writes = 0;
    bool Reference(float& v) override { v = fault == BadReference ? 0 : 3.27f; return true; }
    bool AllOff() override { for (bool& c : connected) c = false; return true; }
    bool Write(uint8_t j, uint16_t c) override {
        ++writes; codes[j] = c; return fault != IoError;
    }
    bool Connect(uint8_t j) override { connected[j] = true; return true; }
    bool Wait(uint32_t) override { return fault != Cancel; }
    bool Cancelled() const override { return fault == Cancel; }
    bool Capture(uint8_t j, uint32_t n, V2CalSamples& s) override {
        double v = 0;
        for (uint8_t source = 0; source < 6; ++source) {
            const bool on = connected[source] || (fault == SwitchClosed && source == 0);
            if (!on || (fault == SwitchOpen && source == 0)) continue;
            if (source == j || (paired && source == 5-j) || (fault == Bridge && j == 1 && source == 0)) {
                v = 5.03 - codes[source] * 0.00245;
                if (fault == BufferHeadroom && source >= 4)
                    v = std::fmax(-4.47, std::fmin(4.90, v));
                if (source == 0) {
                    if (fault == Dead) v = 0;
                    if (fault == Reversed) v = -v;
                    if (fault == Clip) v = std::fmax(-2.0, std::fmin(2.0, v));
                    if (fault == Nonlinear) v += 0.4 * std::sin(codes[source] / 250.0);
                    if (fault == VerifyDrift && writes > 20) v += 0.3;
                }
            }
        }
        s = {};
        for (uint32_t i = 0; i < n; ++i) {
            const double bias = fault == Bias && j == 0 ? 2.3 : 1.66;
            double raw = (bias - v * 0.165) / 3.27 * 65535;
            raw += (i % 2 ? 1 : -1) * (fault == Noisy && j == 0 ? 500 : 2);
            s.Add(static_cast<uint16_t>(std::fmax(0.0, std::fmin(65535.0, raw))));
        }
        return true;
    }

};

int main() {
    { // Buffered DAC endpoint headroom must not reject a linear usable path.
        Rig r; r.fault = Rig::BufferHeadroom; r.paired = true;
        V2Calibration c{}; V2CalReport report;
        assert(V2RunCalibration(r, true, c, report));
        assert(report.channels[4].sweep_v[16] > -4.5f);
        assert(report.channels[4].verification_v < 0.01f);
        assert(report.channels[4].stage == 6);
        assert(report.channels[4].sweep_points == 17);
        assert(report.channels[4].verification_points == 5);
    }
    { // Recalibration replaces old coefficients; it never applies a second fit.
        Rig fresh, already_calibrated;
        V2Calibration a{}, b{}; V2CalReport ra, rb;
        V2CalDesignFallback(b);
        b.jack[0].dac_offset_v = 1234;
        b.jack[0].adc_zero_code = 60000;
        assert(V2RunCalibration(fresh, false, a, ra));
        assert(V2RunCalibration(already_calibrated, false, b, rb));
        assert(a.crc32 == b.crc32);
    }
    for (bool paired : {false, true}) {
        Rig r; r.paired = paired;
        V2Calibration c; V2CalReport report;
        assert(V2RunCalibration(r, paired, c, report));
        assert(V2CalRecordPlausible(c));
        assert(c.crc32 == V2CalComputeCrc(c));
        assert(std::fabs(c.jack[0].dac_gain_v_per_code + 0.00245) < 0.000001);
        assert(report.channels[0].verification_v < 0.01);
        for (bool on : r.connected) assert(!on);
    }
    for (auto fault : {Rig::Dead, Rig::Reversed, Rig::Noisy, Rig::Bias, Rig::Clip,
                       Rig::Nonlinear, Rig::SwitchClosed, Rig::SwitchOpen,
                       Rig::Bridge, Rig::BadReference, Rig::IoError, Rig::Cancel,
                       Rig::VerifyDrift}) {
        Rig r; r.fault = fault;
        V2Calibration c; V2CalReport report;
        assert(!V2RunCalibration(r, false, c, report));
        assert(c.magic == 0 && c.crc32 == 0);
        assert(report.failure != V2CalFailure::None);
        for (bool on : r.connected) assert(!on);
    }
    { // Missing/wrong harness must never pass a paired run.
        Rig r; V2Calibration c; V2CalReport report;
        assert(!V2RunCalibration(r, true, c, report));
        assert(report.failure == V2CalFailure::Isolation);
    }
    V2Calibration c; V2CalDesignFallback(c);
    assert(V2CalRecordPlausible(c));
    for (float bad : {0.0f, 0.002f, -0.00000001f, -1.0f,
                      std::numeric_limits<float>::quiet_NaN(),
                      std::numeric_limits<float>::infinity()}) {
        V2Calibration corrupt = c;
        corrupt.jack[0].dac_gain_v_per_code = bad;
        corrupt.crc32 = V2CalComputeCrc(corrupt); // CRC alone is insufficient.
        assert(!V2CalRecordPlausible(corrupt));
    }
    c.jack[3].dac_min_linear_code = 3500;
    assert(!V2CalRecordPlausible(c));
    std::puts("v2_calibration: nominal/paired, fault injection and record checks passed");
}
