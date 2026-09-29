/**
 * @file v2_factory_cal.cpp
 * @brief V2 factory calibration — bare-metal ADC, loopback sweep, QSPI persist.
 *
 * Uses the shared V2RunCalibration manufacturing checks, with all CV jacks
 * unpatched. Invalid references, I/O errors, noisy/stuck paths, bad fits,
 * switch/isolation faults and verification errors fail before persistence.
 * Normal tolerance and limited endpoint compression are allowed.
 *
 * Bare-metal ADC notes (same rationale as the bench firmware):
 *   - One-shot polled conversions, 16-bit, 810.5-cycle sampling.
 *   - PCSEL and SMPRx are written directly: the HAL channel-config path
 *     requires LL-encoded channel macros and silently corrupts PCSEL when
 *     given raw integers (and the reverse mistake — an LL macro masked
 *     with `& 0x1F` — silently selects channel 0; both bit us during
 *     bring-up).
 *   - VREFINT's channel number is decoded from ADC_CHANNEL_VREFINT rather
 *     than hardcoded, via ADC_CHANNEL_ID_NUMBER_MASK.
 */

// The shared engine lives in this original translation unit so existing
// firmware source lists retain B1+B2 calibration without adding new .cpp files.
#include "alchemy/hw/v2_calibration_runner.h"
#include <algorithm>
#include <cstring>

namespace alchemy {

bool V2RunCalibration(V2CalIo& io, bool paired, V2Calibration& out,
                      V2CalReport& report)
{
    out = {};
    report = {};
    report.jack = 255;
    auto detail = [&](uint8_t metric, double observed, double low, double high, uint8_t measured = 255) {
        report.metric = metric; report.observed = observed;
        report.low = low; report.high = high; report.measured_jack = measured;
    };
    auto fail = [&](V2CalFailure reason) {
        report.failure = io.Cancelled() ? V2CalFailure::Cancelled : reason;
        io.AllOff(); // Never claim cleanup succeeded after an I/O failure.
        out.magic = out.crc32 = 0;
        return false;
    };
    if (!io.AllOff()) return fail(V2CalFailure::Io);
    if (!io.Reference(out.vdda_at_cal) || !V2CalVddaValid(out.vdda_at_cal)) {
        detail(1, out.vdda_at_cal, 3.0, 3.6);
        return fail(V2CalFailure::Reference);
    }
    out.vdda_source = kV2VddaMeasured;
    const double scale = out.vdda_at_cal / (65535.0 * kV2CvInGainDesign);
    double zeros[6] = {};
    auto samples_valid = [&](uint8_t j, const V2CalSamples& s) {
        auto& r = report.channels[j];
        r.noise_v = std::max(r.noise_v, static_cast<float>(s.StdDev() * scale));
        if (V2CalSamplesValid(s, out.vdda_at_cal)) return true;
        if (s.minimum <= 64 || s.maximum >= 65471)
            detail(5, s.minimum <= 64 ? s.minimum : s.maximum, 65, 65470, j);
        else if ((s.maximum - s.minimum)*scale > kV2CalMaxPeakToPeakV)
            detail(4, (s.maximum - s.minimum)*scale, 0, kV2CalMaxPeakToPeakV, j);
        else detail(3, s.StdDev()*scale, 0, kV2CalMaxNoiseV, j);
        report.failure = V2CalFailure::Noise;
        return false;
    };
    auto sample = [&](uint8_t j, uint32_t n, double& volts) {
        V2CalSamples s;
        if (!io.Capture(j, n, s)) return false;
        if (!samples_valid(j, s)) return false;
        volts = (zeros[j] - s.mean) * scale;
        return true;
    };
    if (!io.Wait(20)) return fail(V2CalFailure::Io);
    for (uint8_t j = 0; j < 6; ++j) {
        report.jack = j;
        report.channels[j].stage = 1;
        io.Progress(j, 0);
        V2CalSamples s;
        if (!io.Capture(j, 128, s)) return fail(V2CalFailure::Io);
        report.channels[j].zero_raw = static_cast<float>(s.mean);
        if (!samples_valid(j, s)) return fail(V2CalFailure::Noise);
        if (!V2CalZeroValid(s.mean, out.vdda_at_cal)) {
            detail(2, s.mean*out.vdda_at_cal/65535., 1.35, 1.95, j);
            return fail(V2CalFailure::Zero);
        }
        zeros[j] = s.mean;
        report.channels[j].noise_v = static_cast<float>(s.StdDev() * scale);
        out.jack[j].adc_zero_code = static_cast<uint16_t>(s.mean + 0.5);
    }
    for (uint8_t j = 0; j < 6; ++j) {
        report.jack = j;
        auto& r = report.channels[j];
        r.stage = 2;
        // Changing the disconnected DAC must not appear on any input.
        if (!io.AllOff()) return fail(V2CalFailure::Io);
        for (uint16_t code : {uint16_t(512), uint16_t(3583)}) {
            if (!io.Write(j, code) || !io.Wait(5)) return fail(V2CalFailure::Io);
            for (uint8_t other = 0; other < 6; ++other) {
                double v;
                if (!sample(other, 32, v)) return fail(report.failure == V2CalFailure::Noise ? report.failure : V2CalFailure::Io);
                if (std::fabs(v) > kV2CalMaxResidualV) {
                    detail(6, v, -kV2CalMaxResidualV, kV2CalMaxResidualV, other);
                    return fail(V2CalFailure::Isolation);
                }
            }
        }
        // Stage neutral before connecting. Only one DAC may be connected.
        if (!io.Write(j, 2048) || !io.Connect(j)) return fail(V2CalFailure::Io);
        double x[17], y[17];
        r.stage = 3;
        for (uint8_t k = 0; k < 17; ++k) {
            report.step = k;
            io.Progress(j, k + 1);
            const uint16_t code = (k * 4095u + 8u) / 16u;
            if (!io.Write(j, code) || !io.Wait(5)) return fail(V2CalFailure::Io);
            x[k] = code;
            if (!sample(j, 128, y[k])) return fail(report.failure == V2CalFailure::Noise ? report.failure : V2CalFailure::Io);
            r.sweep_v[k] = static_cast<float>(y[k]);
            r.sweep_points = k + 1;
            // Interior must progress by a meaningful amount; allow only
            // endpoint flattening, never a flat/noisy central fit.
            if (k >= 3 && k <= 14 && y[k - 1] - y[k] < 0.20) {
                detail(9, y[k-1]-y[k], 0.20, 12, j);
                return fail(V2CalFailure::Response);
            }
            if (k && y[k] - y[k - 1] > 0.025) {
                detail(10, y[k]-y[k-1], -12, 0.025, j);
                return fail(V2CalFailure::Response);
            }
            if (k == 4 || k == 12) {
                for (uint8_t other = 0; other < 6; ++other) {
                    if (other == j) continue;
                    double v;
                    if (!sample(other, 64, v)) return fail(report.failure == V2CalFailure::Noise ? report.failure : V2CalFailure::Io);
                    const bool mate = paired && other == 5 - j;
                    const double error = std::fabs(v - (mate ? y[k] : 0.0));
                    if (mate) r.cross_error_v = std::max(r.cross_error_v, static_cast<float>(error));
                    if (error > (mate ? kV2CalCrossErrorV : kV2CalMaxResidualV)) {
                        detail(mate ? 7 : 8, error, 0, mate ? kV2CalCrossErrorV : kV2CalMaxResidualV, other);
                        return fail(V2CalFailure::Isolation);
                    }
                }
            }
        }
        r.span_v = static_cast<float>(y[0] - y[16]);
        if (y[0] < kV2CalMinEndpointV || y[0] > kV2CalMaxEndpointV) {
            detail(11, y[0], kV2CalMinEndpointV, kV2CalMaxEndpointV, j);
            return fail(V2CalFailure::Range);
        }
        if (y[16] > -kV2CalMinEndpointV || y[16] < -kV2CalMaxEndpointV) {
            detail(12, y[16], -kV2CalMaxEndpointV, -kV2CalMinEndpointV, j);
            return fail(V2CalFailure::Range);
        }
        r.stage = 4;
        double sx=0, sy=0, sxx=0, sxy=0;
        for (uint8_t k = 2; k <= 14; ++k) {
            sx += x[k]; sy += y[k]; sxx += x[k]*x[k]; sxy += x[k]*y[k];
        }
        const double slope = (13*sxy - sx*sy) / (13*sxx - sx*sx);
        const double offset = (sy - slope*sx) / 13;
        auto& c = out.jack[j];
        c.dac_gain_v_per_code = static_cast<float>(slope);
        c.dac_offset_v = static_cast<float>(offset);
        uint8_t lo=2, hi=14;
        auto residual = [&](uint8_t k) { return std::fabs(y[k] - (offset+slope*x[k])); };
        for (uint8_t k = 2; k <= 14; ++k) {
            r.residual_v = std::max(r.residual_v, static_cast<float>(residual(k)));
            if (residual(k) > kV2CalMaxResidualV) {
                detail(13, residual(k), 0, kV2CalMaxResidualV, j);
                return fail(V2CalFailure::Fit);
            }
        }
        while (lo > 0 && residual(lo-1) <= kV2CalMaxResidualV) --lo;
        while (hi < 16 && residual(hi+1) <= kV2CalMaxResidualV) ++hi;
        c.dac_min_linear_code = static_cast<uint16_t>(x[lo]);
        c.dac_max_linear_code = static_cast<uint16_t>(x[hi]);
        if (!V2CalJackValid(c, out.vdda_at_cal)) {
            detail(14, slope, kV2CvOutGainDesign*kV2VddaDesign/4096.0f*1.2f,
                   kV2CvOutGainDesign*kV2VddaDesign/4096.0f*0.8f, j);
            return fail(V2CalFailure::Fit);
        }
        // Fresh, non-fit points in alternating order exercise settling and
        // inversion of the actual candidate coefficients.
        r.stage = 5;
        for (double target : {3.7, -3.7, 1.3, -1.3, 0.0}) {
            const double code_f = (target - offset) / slope;
            if (!(code_f >= c.dac_min_linear_code && code_f <= c.dac_max_linear_code))
                return fail(V2CalFailure::Range);
            if (!io.Write(j, static_cast<uint16_t>(code_f + 0.5)) || !io.Wait(5))
                return fail(V2CalFailure::Io);
            double v;
            if (!sample(j, 128, v)) return fail(report.failure == V2CalFailure::Noise ? report.failure : V2CalFailure::Io);
            r.verification_v = std::max(r.verification_v, static_cast<float>(std::fabs(v-target)));
            ++r.verification_points;
            if (std::fabs(v-target) > kV2CalMaxResidualV) {
                detail(15, v, target-kV2CalMaxResidualV, target+kV2CalMaxResidualV, j);
                return fail(V2CalFailure::Verification);
            }
        }
        if (!io.AllOff() || !io.Write(j, 2048)) return fail(V2CalFailure::Io);
        r.stage = 6;
    }
    report.jack = 255;
    if (!V2CalRecordPlausible(out)) return fail(V2CalFailure::Fit);
    out.magic = kV2CalMagic;
    out.schema_version = kV2CalSchemaVersion;
    out.crc32 = V2CalComputeCrc(out);
    return true;
}

} // namespace alchemy

// Host tests exercise the same engine above without linking ADC/LED/QSPI HAL.
#if !defined(ALCHEMY_CALIBRATION_HOST_TEST)
#include "alchemy/hw/v2_factory_cal.h"

#include <cmath>
#include <cstring>

#include "daisy_seed.h"
#include "stm32h7xx_hal.h"

#include "alchemy/hw/alchemy_lab_v2.h"
#include "alchemy/hw/v2_calibration.h"
#include "alchemy/hw/v2_calibration_runner.h"

namespace alchemy {

namespace {

constexpr uint32_t kVrefintCalAddr = 0x1FF1E860u;
constexpr float kVrefintCalVdda = 3.30f;

/* ── Bare-metal one-shot ADC (ADC1 jacks + ADC3 VREFINT) ─────────────── */

ADC_HandleTypeDef s_hadc1 = {};
ADC_HandleTypeDef s_hadc3 = {};

bool AdcInitOne(ADC_HandleTypeDef& h, ADC_TypeDef* instance)
{
    h.Instance                      = instance;
    h.Init.ClockPrescaler           = ADC_CLOCK_ASYNC_DIV2;
    h.Init.Resolution               = ADC_RESOLUTION_16B;
    h.Init.ScanConvMode             = ADC_SCAN_DISABLE;
    h.Init.EOCSelection             = ADC_EOC_SINGLE_CONV;
    h.Init.LowPowerAutoWait         = DISABLE;
    h.Init.ContinuousConvMode       = DISABLE;
    h.Init.NbrOfConversion          = 1;
    h.Init.DiscontinuousConvMode    = DISABLE;
    h.Init.ExternalTrigConv         = ADC_SOFTWARE_START;
    h.Init.ExternalTrigConvEdge     = ADC_EXTERNALTRIGCONVEDGE_NONE;
    h.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DR;
    h.Init.Overrun                  = ADC_OVR_DATA_OVERWRITTEN;
    h.Init.LeftBitShift             = ADC_LEFTBITSHIFT_NONE;
    h.Init.OversamplingMode         = DISABLE;

    if (HAL_ADC_Init(&h) != HAL_OK) return false;
    return HAL_ADCEx_Calibration_Start(&h, ADC_CALIB_OFFSET_LINEARITY,
                                       ADC_SINGLE_ENDED) == HAL_OK;
}

bool CalAdcInit()
{
    /* Jack ADC pins to analog mode (daisy GPIO handles port clocks). */
    for (uint8_t i = 0; i < kNumCvInputs; ++i)
    {
        daisy::GPIO g;
        g.Init(kCvPins[i], daisy::GPIO::Mode::ANALOG);
    }

    __HAL_RCC_ADC12_CLK_ENABLE();
    __HAL_RCC_ADC3_CLK_ENABLE();

    if (!AdcInitOne(s_hadc1, ADC1)) return false;
    if (!AdcInitOne(s_hadc3, ADC3)) return false;

    /* PCSEL + max sampling time, written directly (see file header). */
    uint32_t pcsel = 0;
    for (uint8_t i = 0; i < kNumCvInputs; ++i)
        pcsel |= (1u << kCvAdc1Channels[i]);
    ADC1->PCSEL = pcsel;
    ADC1->SMPR1 = 0x3FFFFFFFu;  /* 810.5 cycles on channels 0-9   */
    ADC1->SMPR2 = 0x3FFFFFFFu;  /* 810.5 cycles on channels 10-19 */

    /* VREFINT on ADC3: decode the channel from the HAL macro. */
    constexpr uint32_t vrefint_ch =
        (ADC_CHANNEL_VREFINT & ADC_CHANNEL_ID_NUMBER_MASK)
        >> ADC_CHANNEL_ID_NUMBER_BITOFFSET_POS;
    ADC3->PCSEL      |= (1u << vrefint_ch);
    ADC3->SMPR2      |= (0b111u << ((vrefint_ch - 10u) * 3u));
    ADC3_COMMON->CCR |= ADC_CCR_VREFEN;
    return true;
}

bool AdcReadSingle(ADC_HandleTypeDef& h, uint32_t channel, uint16_t& out)
{
    h.Instance->SQR1 = ((channel & 0x1Fu) << ADC_SQR1_SQ1_Pos);
    if (HAL_ADC_Start(&h) != HAL_OK)                  return false;
    if (HAL_ADC_PollForConversion(&h, 10) != HAL_OK) {
        HAL_ADC_Stop(&h);
        return false;
    }
    out = static_cast<uint16_t>(HAL_ADC_GetValue(&h));
    HAL_ADC_Stop(&h);
    return true;
}

bool ReadJackRaw(uint8_t jack, uint16_t& out)
{
    return AdcReadSingle(s_hadc1, kCvAdc1Channels[jack], out);
}

bool ReadVrefintRaw(uint16_t& out)
{
    constexpr uint32_t ch =
        (ADC_CHANNEL_VREFINT & ADC_CHANNEL_ID_NUMBER_MASK)
        >> ADC_CHANNEL_ID_NUMBER_BITOFFSET_POS;
    return AdcReadSingle(s_hadc3, ch, out);
}

/* ── DAC + routing helpers (drive the board's own members) ───────────── */

uint16_t s_mcp_shadow[4] = { 2048u, 2048u, 2048u, 2048u };

bool WriteJackDac(AlchemyLabV2& hw, uint8_t jack, uint16_t code)
{
    code = static_cast<uint16_t>(code & 0x0FFFu);
    const DacRoute& r = kDacRouting[jack];
    const uint8_t src = static_cast<uint8_t>(r.source);
    if (src < 4u)
    {
        s_mcp_shadow[src] = code;
        return hw.dac.WriteAll(s_mcp_shadow[0], s_mcp_shadow[1],
                              s_mcp_shadow[2], s_mcp_shadow[3])
            && hw.dac.PulseLdac(hw.expander, kPca9557IoLdac);
    }
    else
    {
        return hw.stm_dac.WriteValue((src == 4u) ? daisy::DacHandle::Channel::ONE
                                          : daisy::DacHandle::Channel::TWO,
                              code) == daisy::DacHandle::Result::OK;
    }
}

bool Dg411AllOff(AlchemyLabV2& hw)
{
    return hw.expander.WriteOutputs(kPca9557OutputBoot);
}

/* ── QSPI persist (same dance as the bench cal_writer) ───────────────── */

daisy::QSPIHandle::Config QspiConfig(daisy::QSPIHandle::Config::Mode mode)
{
    /* Mirrors DaisySeed::ConfigureQspi — fixed pins on the Seed module. */
    daisy::QSPIHandle::Config q;
    q.device = daisy::QSPIHandle::Config::Device::IS25LP064A;
    q.mode   = mode;
    q.pin_config.io0 = daisy::Pin(daisy::PORTF, 8);
    q.pin_config.io1 = daisy::Pin(daisy::PORTF, 9);
    q.pin_config.io2 = daisy::Pin(daisy::PORTF, 7);
    q.pin_config.io3 = daisy::Pin(daisy::PORTF, 6);
    q.pin_config.clk = daisy::Pin(daisy::PORTF, 10);
    q.pin_config.ncs = daisy::Pin(daisy::PORTG, 6);
    return q;
}

bool PersistToQspi(AlchemyLabV2& hw, const V2Calibration& cal)
{
    if (cal.magic != kV2CalMagic || cal.schema_version != kV2CalSchemaVersion
        || cal.vdda_source != kV2VddaMeasured || !V2CalRecordPlausible(cal)
        || cal.crc32 != V2CalComputeCrc(cal)) return false;
    auto& qspi = hw.seed.qspi;
    const auto indirect = QspiConfig(daisy::QSPIHandle::Config::Mode::INDIRECT_POLLING);
    const auto mapped = QspiConfig(daisy::QSPIHandle::Config::Mode::MEMORY_MAPPED);
    const bool init_ok = qspi.Init(indirect) == daisy::QSPIHandle::Result::OK;
    bool ok = init_ok;
    if (ok) ok = qspi.Erase(kV2CalQspiAddr, kV2CalQspiAddr + kV2CalQspiSectorSize)
                     == daisy::QSPIHandle::Result::OK;
    if (ok) ok = qspi.Write(kV2CalQspiAddr, sizeof(cal),
                           reinterpret_cast<uint8_t*>(const_cast<V2Calibration*>(&cal)))
                     == daisy::QSPIHandle::Result::OK;
    // Always attempt to restore the mapped mode, including erase/write failure.
    const bool mapped_ok = qspi.Init(mapped) == daisy::QSPIHandle::Result::OK;
    if (!ok || !mapped_ok) return false;
    V2Calibration readback{};
    return V2CalLoadFromQspi(readback) && std::memcmp(&cal, &readback, sizeof(cal)) == 0;
}

/* ── Panel feedback ──────────────────────────────────────────────────── */
/*
 * The 102-LED panel narrates the whole procedure. Ring k mirrors jack k
 * (ring 0 ↔ J3 … ring 5 ↔ J8):
 *
 *   boot ack   : B1 + B2 accent pairs white (your hold registered)
 *   VREFINT    : all button pairs amber
 *   zero pass  : rings fill BLUE one after another (wave across panel)
 *   DAC sweeps : active jack's ring fills AMBER point-by-point with a
 *                bright head pixel; finished jacks hold dim GREEN
 *   persist    : button pairs white (panel holds latched frame — the
 *                WS2812s keep displaying with zero data traffic while
 *                QSPI leaves memory-mapped mode)
 *   success    : whole panel flashes green ×3, holds green till release
 *   failure    : whole panel dim red; the jack that failed (if any)
 *                blinks bright red — walk straight to the bad channel
 *
 * Frames are pushed during DAC settle windows (DMA ~3 ms < 5 ms settle),
 * so the LED data line is always quiet while the ADC samples.
 *
 * Colors are pre-dimmed: full-panel holds stay under ~300 mA.
 */

using Rgb = LedPanel::Rgb;

constexpr Rgb kLedAmber    {120,  60,   0};
constexpr Rgb kLedAmberHead{210, 120,  10};
constexpr Rgb kLedBlue     { 16,  40, 120};
constexpr Rgb kLedGreen    {  0, 140,  30};
constexpr Rgb kLedGreenHold{  0,  48,  10};
constexpr Rgb kLedRed      {170,  16,  16};
constexpr Rgb kLedRedDim   { 40,   6,   6};
constexpr Rgb kLedWhite    { 70,  70,  70};

/* Which jack was being swept when a failure occurred (0xFF = none). */
uint8_t s_fail_jack = 0xFFu;

void LedsShow(AlchemyLabV2& hw)
{
    while (hw.leds.Busy())
        daisy::System::Delay(1);
    hw.leds.Show();
}

void RingSolid(AlchemyLabV2& hw, uint8_t ring, const Rgb& c)
{
    for (uint8_t off = 0; off < kLedsPerRing; ++off)
        hw.leds.SetRingByOffset(ring, off, c);
}

void AllRings(AlchemyLabV2& hw, const Rgb& c)
{
    for (uint8_t r = 0; r < kNumLedRings; ++r)
        RingSolid(hw, r, c);
}

void AllButtons(AlchemyLabV2& hw, const Rgb& c)
{
    for (uint8_t b = 0; b < kNumButtons; ++b)
        hw.leds.SetButtonPair(b, c);
}

/** Active-sweep ring: n_lit progress pixels, brighter head. */
void RingProgress(AlchemyLabV2& hw, uint8_t ring, uint8_t n_lit)
{
    hw.leds.ClearRing(ring);
    for (uint8_t off = 0; off < n_lit && off < kLedsPerRing; ++off)
        hw.leds.SetRingByOffset(ring, off,
                                (off + 1u == n_lit) ? kLedAmberHead : kLedAmber);
}

[[noreturn]] void FailForever(AlchemyLabV2& hw)
{
    /* Whole panel dim red; the failing jack's ring (when known) blinks
     * bright red so the bad channel is identifiable from across the
     * room. The previous QSPI record is only touched after every sweep
     * succeeded, so failure here leaves earlier calibration intact
     * (a torn QSPI write fails CRC on next boot → clean fallback). */
    Dg411AllOff(hw); // Best effort; a failed expander cannot guarantee disconnection.
    bool phase = false;
    while (true)
    {
        hw.leds.Clear();
        AllRings(hw, kLedRedDim);
        AllButtons(hw, kLedRed);
        if (s_fail_jack < kNumLedRings && phase)
            RingSolid(hw, s_fail_jack, kLedRed);
        LedsShow(hw);
        hw.seed.SetLed(phase);
        phase = !phase;
        daisy::System::Delay(160);
    }
}

/* ── The procedure ───────────────────────────────────────────────────── */

class FactoryIo final : public V2CalIo
{
  public:
    explicit FactoryIo(AlchemyLabV2& hw) : hw_(hw) {}
    bool Reference(float& vdda) override
    {
        const uint16_t word = *reinterpret_cast<const volatile uint16_t*>(kVrefintCalAddr);
        if (word == 0 || word == 65535) return false;
        V2CalSamples samples;
        for (uint32_t i = 0; i < 500; ++i) {
            uint16_t raw;
            if (!ReadVrefintRaw(raw) || raw == 0 || raw == 65535) return false;
            samples.Add(raw);
        }
        if (samples.StdDev() > samples.mean * 0.01) return false;
        vdda = kVrefintCalVdda * word / samples.mean;
        return V2CalVddaValid(vdda);
    }
    bool AllOff() override { return Dg411AllOff(hw_); }
    bool Write(uint8_t j, uint16_t code) override { return WriteJackDac(hw_, j, code); }
    bool Connect(uint8_t j) override
    {
        return hw_.expander.SetOutputBit(kDacRouting[j].select_io, kDg411OnLevel);
    }
    bool Capture(uint8_t j, uint32_t n, V2CalSamples& out) override
    {
        out = {};
        for (uint32_t i = 0; i < n; ++i) {
            uint16_t raw;
            if (!ReadJackRaw(j, raw)) return false;
            out.Add(raw);
        }
        return true;
    }
    bool Wait(uint32_t ms) override { daisy::System::Delay(ms); return true; }
    void Progress(uint8_t j, uint8_t step) override
    {
        s_fail_jack = j;
        if (!step) RingSolid(hw_, j, kLedBlue);
        else RingProgress(hw_, j, (step * kLedsPerRing) / 17);
        LedsShow(hw_);
        daisy::System::Delay(5); // Quiet LED DMA before ADC acquisition.
    }
  private:
    AlchemyLabV2& hw_;
};

bool RunProcedure(AlchemyLabV2& hw, V2Calibration& out)
{
    FactoryIo io(hw);
    V2CalReport report;
    AllButtons(hw, kLedAmber);
    LedsShow(hw);
    daisy::System::Delay(10);
    const bool ok = V2RunCalibration(io, false, out, report);
    s_fail_jack = report.jack;
    return ok;
}

}  // namespace

bool V2CalibrationAdcInit() { return CalAdcInit(); }
bool V2CalibrationReadJack(uint8_t jack, uint16_t& raw)
{
    return jack < kNumCvInputs && ReadJackRaw(jack, raw);
}
bool V2CalibrationReadReference(uint16_t& raw) { return ReadVrefintRaw(raw); }

bool V2PersistCalibration(AlchemyLabV2& hw, const V2Calibration& cal)
{
    return PersistToQspi(hw, cal);
}

/* ── Public entry points ─────────────────────────────────────────────── */

bool V2FactoryCalRequested()
{
    daisy::GPIO b1, b2;
    b1.Init(kOnMcuBtnPins[0], daisy::GPIO::Mode::INPUT, daisy::GPIO::Pull::PULLUP);
    b2.Init(kOnMcuBtnPins[1], daisy::GPIO::Mode::INPUT, daisy::GPIO::Pull::PULLUP);
    daisy::System::Delay(2);  /* pull-up settle */
    /* Active LOW. Require both, sampled twice 30 ms apart (debounce). */
    if (b1.Read() || b2.Read()) return false;
    daisy::System::Delay(30);
    return !b1.Read() && !b2.Read();
}

[[noreturn]] void V2RunFactoryCalibrationAndReset(AlchemyLabV2& hw)
{
    /* Panel first, so even bring-up failures get the red display. The
     * white B1+B2 pairs acknowledge the operator's button hold. */
    hw.strip.Init(kLedTotal);
    hw.leds.Init(hw.strip, kAlchemyLabV2Layout);
    hw.leds.SetButtonPair(kButtonB1, kLedWhite);
    hw.leds.SetButtonPair(kButtonB2, kLedWhite);
    LedsShow(hw);

    /* Bring up exactly the peripherals the procedure needs, on the
     * board's own members. Init() hasn't configured them yet (this runs
     * at the top of Init), and we soft-reset on the way out, so there is
     * no double-init hazard. */
    daisy::I2CHandle::Config i2c_cfg;
    i2c_cfg.periph         = daisy::I2CHandle::Config::Peripheral::I2C_1;
    i2c_cfg.speed          = daisy::I2CHandle::Config::Speed::I2C_400KHZ;
    i2c_cfg.mode           = daisy::I2CHandle::Config::Mode::I2C_MASTER;
    i2c_cfg.pin_config.scl = daisy::Pin(daisy::PORTB, 8);
    i2c_cfg.pin_config.sda = daisy::Pin(daisy::PORTB, 9);
    if (hw.i2c.Init(i2c_cfg) != daisy::I2CHandle::Result::OK) FailForever(hw);

    if (!hw.expander.Init(hw.i2c, kPca9557Address))           FailForever(hw);
    if (!hw.dac.Init(hw.i2c, kMcp4728AddrFirst, kMcp4728AddrLast))
        FailForever(hw);
    if (!hw.dac.PulseLdac(hw.expander, kPca9557IoLdac)) FailForever(hw);

    daisy::DacHandle::Config dac_cfg;
    dac_cfg.target_samplerate = 48000u;
    dac_cfg.chn               = daisy::DacHandle::Channel::BOTH;
    dac_cfg.mode              = daisy::DacHandle::Mode::POLLING;
    dac_cfg.bitdepth          = daisy::DacHandle::BitDepth::BITS_12;
    dac_cfg.buff_state        = daisy::DacHandle::BufferState::ENABLED;
    if (hw.stm_dac.Init(dac_cfg) != daisy::DacHandle::Result::OK)
        FailForever(hw);
    if (hw.stm_dac.WriteValue(daisy::DacHandle::Channel::ONE, 2048u) != daisy::DacHandle::Result::OK
        || hw.stm_dac.WriteValue(daisy::DacHandle::Channel::TWO, 2048u) != daisy::DacHandle::Result::OK)
        FailForever(hw);

    if (!CalAdcInit()) FailForever(hw);

    V2Calibration cal{};
    if (!RunProcedure(hw, cal)) FailForever(hw);

    /* Persist. Button pairs go white; the rings hold their latched
     * frame with zero data traffic while QSPI leaves memory-mapped
     * mode for the erase + write. */
    AllButtons(hw, kLedWhite);
    LedsShow(hw);
    daisy::System::Delay(5);
    if (!PersistToQspi(hw, cal)) FailForever(hw);

    /* Success: triple green flash, then solid green until the operator
     * releases both buttons (so the reboot doesn't re-enter cal). */
    hw.seed.SetLed(true);
    for (uint8_t i = 0; i < 3u; ++i)
    {
        hw.leds.Clear();
        AllRings(hw, kLedGreen);
        AllButtons(hw, kLedGreen);
        LedsShow(hw);
        daisy::System::Delay(180);
        hw.leds.Clear();
        LedsShow(hw);
        daisy::System::Delay(140);
    }
    hw.leds.Clear();
    AllRings(hw, kLedGreenHold);
    AllButtons(hw, kLedGreen);
    LedsShow(hw);

    daisy::GPIO b1, b2;
    b1.Init(kOnMcuBtnPins[0], daisy::GPIO::Mode::INPUT, daisy::GPIO::Pull::PULLUP);
    b2.Init(kOnMcuBtnPins[1], daisy::GPIO::Mode::INPUT, daisy::GPIO::Pull::PULLUP);
    while (!b1.Read() || !b2.Read())
        daisy::System::Delay(10);
    hw.seed.SetLed(false);
    hw.leds.Clear();
    LedsShow(hw);
    daisy::System::Delay(100);

    NVIC_SystemReset();
    while (true) {}  /* unreachable; satisfies [[noreturn]] on all paths */
}

} // namespace alchemy

#endif
