/**
 * @file v2_cal_test.cpp
 * @brief Alchemy Lab V2 — scope-driven calibration acceptance test.
 *
 * All six CV jacks output the same precision voltage. Controls:
 *
 *   B1 — cycle target voltage in 1 V steps: −5, −4, ... +4, +5, wrap.
 *   B2 — toggle CALIBRATED vs UNCALIBRATED output.
 *
 * ── Panel display ──────────────────────────────────────────────────────
 * Every ring is a voltmeter for its jack (ring 0 ↔ J3 … ring 5 ↔ J8):
 *
 *   - White anchor pixel at 12 o'clock on every ring.
 *   - |target| pixels counted off from the anchor — CLOCKWISE for
 *     positive volts, COUNTER-CLOCKWISE for negative. +3 V = three
 *     pixels clockwise; read the voltage by counting lit pixels.
 *   - Pixel color = mode:  GREEN(+)/RED(−) = CALIBRATED,
 *     AMBER (both polarities) = UNCALIBRATED.
 *
 * Button accents:  B1 pair dim white (step). B2 pair = mode: cyan when
 * CALIBRATED, red when UNCALIBRATED. B3 pair = cal record status: green
 * when a real QSPI record loaded, red when running on design fallback.
 *
 * Seed LED mirrors mode (on = calibrated), same as before.
 *
 * The panel only refreshes on a button press (or at 2 Hz while a clamp
 * blink is active), keeping the WS2812 data line quiet for clean scope
 * measurements.
 *
 * Each button press also emits a HostLink diagnostic message (open the
 * programmer Device console or hostlink-cli logs --follow):
 *   target, mode, and per-jack DAC code + cal-vs-design delta in volts.
 *
 * To (re)calibrate: hold B1 + B2 through a reset. Factory cal runs
 * (~15 s, panel narrates each phase), persists to QSPI, reboots into
 * this firmware with cal live.
 */

#include "alchemy/hw/alchemy_lab_v2.h"
#include "alchemy/host_link/host.h"
#include "alchemy/host_link/diagnostics.h"

using namespace alchemy;

static alchemy::hostlink::Diagnostics debug;
static alchemy::hostlink::Host host("v2_cal_test", "V2 Calibration Test", "1.0.0", "example");
static AlchemyLabV2 hw;

/* −5 .. +5 in 1 V steps. Start index 5 = 0 V. */
static constexpr float kStepV[] = {
    -5.f, -4.f, -3.f, -2.f, -1.f, 0.f, 1.f, 2.f, 3.f, 4.f, 5.f
};
static constexpr size_t kNumSteps = sizeof(kStepV) / sizeof(kStepV[0]);

/* Panel palette (arcs are ≤6 px/ring, accents 2 px — bright is fine). */
using Rgb = LedPanel::Rgb;
static constexpr Rgb kColPos    {  0, 160,  40};  /* calibrated, positive  */
static constexpr Rgb kColNeg    {180,  20,  20};  /* calibrated, negative  */
static constexpr Rgb kColUncal  {170,  90,   0};  /* uncalibrated, either  */
static constexpr Rgb kColAnchor { 60,  60,  60};  /* 12 o'clock reference  */
static constexpr Rgb kColCyan   {  0, 120, 140};  /* B2: calibrated mode   */
static constexpr Rgb kColRed    {170,  16,  16};  /* B2/B3: warning        */
static constexpr Rgb kColGreen  {  0, 110,  25};  /* B3: record loaded     */
static constexpr Rgb kColWhite  { 50,  50,  50};  /* B1: step hint         */

/* Design-nominal (uncalibrated) DAC code for a target jack voltage:
 *   V_jack = 5 − 3.03 × (code / 4096 × 3.30)  →  invert for code. */
static uint16_t DesignCode(float v_target)
{
    float code = (v_target - kV2CvOutOffsetDesignV)
                 / (kV2CvOutGainDesign * kV2VddaDesign / 4096.0f);
    if (code < 0.0f)    code = 0.0f;
    if (code > 4095.0f) code = 4095.0f;
    return static_cast<uint16_t>(code + 0.5f);
}

/* Calibrated code, mirroring CvJack::SetVolts' math (for delta + clamp UI). */
static uint16_t CalCode(uint8_t jack, float v_target)
{
    const V2JackCal& jc = hw.Calibration().jack[jack];
    float code = (v_target - jc.dac_offset_v) / jc.dac_gain_v_per_code;
    if (code < jc.dac_min_linear_code) code = jc.dac_min_linear_code;
    if (code > jc.dac_max_linear_code) code = jc.dac_max_linear_code;
    return static_cast<uint16_t>(code + 0.5f);
}

static bool IsClamped(uint8_t jack, float v_target)
{
    const V2JackCal& jc = hw.Calibration().jack[jack];
    const uint16_t  cc = CalCode(jack, v_target);
    return cc == jc.dac_min_linear_code || cc == jc.dac_max_linear_code;
}

/* ── Panel voltmeter ───────────────────────────────────────────────────── */

static bool s_any_clamped = false;  /* drives the 2 Hz blink redraw */

static void DrawPanel(size_t step, bool calibrated, bool blink_phase)
{
    const float v     = kStepV[step];
    const int   n     = static_cast<int>((v < 0 ? -v : v) + 0.5f);
    const bool  pos   = v > 0.0f;

    const Rgb volt_col = calibrated ? (pos ? kColPos : kColNeg) : kColUncal;

    hw.leds.Clear();
    s_any_clamped = false;

    for (uint8_t ring = 0; ring < kNumLedRings; ++ring)
    {
        const bool clamped = calibrated && IsClamped(ring, v);
        if (clamped) s_any_clamped = true;

        /* Anchor at noon — always on (the "0 V" reference mark). */
        hw.leds.SetRingByHour(ring, 0.0f, kColAnchor);

        /* Clamp blink: the volt pixels strobe; the anchor stays. */
        if (clamped && !blink_phase)
            continue;

        /* One pixel per volt, stepping away from noon. The ring has 16
         * LEDs (0.75 h each); use one LED per step so counts read
         * directly: hour = ±0.75 × k. */
        for (int k = 1; k <= n; ++k)
        {
            const float hour = pos ? (0.75f * k) : (12.0f - 0.75f * k);
            hw.leds.SetRingByHour(ring, hour, volt_col);
        }
    }

    hw.leds.SetButtonPair(kButtonB1, kColWhite);
    hw.leds.SetButtonPair(kButtonB2, calibrated ? kColCyan : kColRed);
    hw.leds.SetButtonPair(kButtonB3, hw.IsCalibrated() ? kColGreen : kColRed);

    while (hw.leds.Busy())
        daisy::System::Delay(1);
    hw.leds.Show();
}

/* ── Outputs + CDC status ──────────────────────────────────────────────── */

static void ApplyOutputs(size_t step, bool calibrated)
{
    const float v = kStepV[step];

    if (calibrated)
    {
        for (uint8_t j = 0; j < kNumDacOuts; ++j)
            hw.cv_jacks[j].SetVolts(v);
    }
    else
    {
        /* Raw design-code writes, bypassing calibration entirely. */
        const uint16_t code = DesignCode(v);
        hw.dac.WriteAll(code, code, code, code);
        hw.dac.PulseLdac(hw.expander, kPca9557IoLdac);
        hw.stm_dac.WriteValue(daisy::DacHandle::Channel::ONE, code);
        hw.stm_dac.WriteValue(daisy::DacHandle::Channel::TWO, code);
    }

    hw.seed.SetLed(calibrated);
    DrawPanel(step, calibrated, true);

    /* One status burst per press — small enough for live CDC. */
    const uint16_t dcode = DesignCode(v);
    debug.PrintLine("%s", "");
    daisy::System::Delay(100);
    debug.PrintLine("Target %s%d.00 V  |  %s  |  design_code=%u",
                      (v < 0) ? "-" : "+",
                      (int)((v < 0) ? -v : v),
                      calibrated ? "CALIBRATED" : "UNCALIBRATED",
                      (unsigned)dcode);
    daisy::System::Delay(100);
    if (calibrated)
    {
        for (uint8_t j = 0; j < kNumDacOuts; ++j)
        {
            const uint16_t cc = CalCode(j, v);
            const float delta_v =
                (float)((int32_t)cc - (int32_t)dcode)
                * hw.Calibration().jack[j].dac_gain_v_per_code;
            /* Manual fixed-point print: PrintLine's %f support varies. */
            const int mv = (int)(delta_v * 1000.0f + (delta_v >= 0 ? 0.5f : -0.5f));
            debug.PrintLine("  J%d: code=%4u  delta=%+d mV%s",
                              j + 3, (unsigned)cc, mv,
                              IsClamped(j, v)
                                  ? "  (clamped at measured range)" : "");
            daisy::System::Delay(100);
        }
    }
}

int main()
{
    hw.Init();
    host.Extend(debug);

    for (uint8_t j = 0; j < kNumDacOuts; ++j)
        hw.cv_jacks[j].EnableCvOutput();

    daisy::System::Delay(500);
    debug.PrintLine("=== V2 CAL TEST ===  IsCalibrated=%d  (B1: step volts, B2: cal on/off)",
                      (int)hw.IsCalibrated());

    size_t step       = 5;     /* 0 V */
    bool   calibrated = true;
    ApplyOutputs(step, calibrated);

    bool     blink_phase = true;
    uint32_t last_blink  = daisy::System::GetNow();

    while (true)
    {
        host.Poll(daisy::System::GetNow());
        hw.ProcessAllControls();

        if (hw.buttons[kButtonB1].RisingEdge())
        {
            step = (step + 1u) % kNumSteps;
            ApplyOutputs(step, calibrated);
        }
        if (hw.buttons[kButtonB2].RisingEdge())
        {
            calibrated = !calibrated;
            ApplyOutputs(step, calibrated);
        }

        /* 2 Hz clamp strobe — the only time the panel refreshes without
         * a button press, so the LED data line stays scope-quiet. */
        const uint32_t now = daisy::System::GetNow();
        if (s_any_clamped && now - last_blink >= 250u)
        {
            last_blink  = now;
            blink_phase = !blink_phase;
            DrawPanel(step, calibrated, blink_phase);
        }

        daisy::System::Delay(1);  /* ~1 kHz control poll */
    }
}
