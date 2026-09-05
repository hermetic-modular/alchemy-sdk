/**
 * @file alchemy_lab_v2.h
 * @brief alchemy::AlchemyLabV2 — Alchemy Lab V2 board class.
 *
 * Production surface (used by the unified `alchemy::AlchemyLab` typedef and
 * shared firmware):
 *   - seed, pots[kNumPots]
 *   - buttons[kNumButtons]   uniform IButton& accessor (B1/B2 on-MCU, B3
 *                            routed through the PCA9557 expander)
 *   - strip, leds            WS2812 + LedPanel
 *   - sdmmc, fatfs           SDMMC1 (1-bit, MEDIUM_SLOW) + FatFS volume "0:"
 *   - Init(), ProcessAllControls(), StartAudio(), FlushCvOutputs()
 *   - Layout(), Arc(), SampleRate(), BlockSize()
 *   - I2cReady(), ExpanderReady(), Mcp4728Ready(), StmDacReady()
 *
 * Jacks — see README "Jack reference" for the full per-jack table:
 *   - triggers[2]   J1, J2   codec inputs, AC-coupled. RisingEdge().
 *   - cv_jacks[8]   J3..J10. Full CvJack API on every entry.
 *                   [0..5] = J3..J8 (switchable analog, DG411-routed)
 *                   [6, 7] = J9, J10 (codec out, DC-coupled)
 *   - j1..j10       named refs into the arrays above.
 *
 * Frozen-contract compatibility:
 *   - cv[kNumCvInputs] is a thin operator[] proxy over cv_jacks[0..5], so
 *     framework code that does `hw.cv[i].Value()` keeps working unchanged.
 *
 * Calibration (see v2_calibration.h / v2_factory_cal.h):
 *   - Init() loads the per-board V2Calibration record from QSPI; missing
 *     or corrupt records fall back to design constants silently.
 *   - cv_jacks[i].Volts() and SetVolts() are calibrated automatically.
 *   - IsCalibrated() reports whether a real record was loaded.
 *   - Holding B1 + B2 through boot runs the on-board factory cal
 *     procedure and soft-resets (see v2_factory_cal.h).
 *
 * Audio shim: StartAudio() installs a thin wrapper around the user
 * callback that (a) feeds J1/J2 sample blocks to the trigger detectors
 * and (b) fills out[0]/out[1] with the staged SetVolts target for any
 * codec jack with EnableCvOutput() in effect. A user callback that
 * doesn't touch out[0]/out[1] (or whose writes are about to be
 * overwritten anyway) sees zero behavioural change. Single-instance:
 * one AlchemyLabV2 per program — the shim uses a static instance ptr.
 */

#pragma once

#include "daisy_seed.h"
#include "per/i2c.h"
#include "per/dac.h"
#include "per/sdmmc.h"
#include "sys/fatfs.h"

#include "alchemy/hw/alchemy_lab_v2_layout.h"
#include "alchemy/hw/button.h"
#include "alchemy/hw/cv_input.h"
#include "alchemy/hw/cv_jack.h"
#include "alchemy/hw/i_button.h"
#include "alchemy/hw/trigger_jack.h"
#include "alchemy/hw/ws2812.h"
#include "alchemy/hw/pca9557.h"
#include "alchemy/hw/pca9557_button.h"
#include "alchemy/hw/mcp4728.h"
#include "alchemy/hw/v2_calibration.h"

#include "alchemy/led/panel.h"

namespace alchemy {

class AlchemyLabV2
{
  public:
    /* ── Hardware peripherals (Daisy idiom: public members) ─────────────── */

    daisy::DaisySeed       seed;
    daisy::AnalogControl   pots[kNumPots];
    Button                 on_mcu_buttons[kNumOnMcuButtons];   ///< B1, B2
    Pca9557Button          b3;                                 ///< B3 via expander

    struct ButtonArray
    {
        IButton* slots[kNumButtons];
        IButton&       operator[](uint8_t i)       { return *slots[i]; }
        const IButton& operator[](uint8_t i) const { return *slots[i]; }
    };
    ButtonArray buttons {{ &on_mcu_buttons[0], &on_mcu_buttons[1], &b3 }};

    Ws2812Strip            strip;
    LedPanel               leds;

    daisy::I2CHandle       i2c;
    Pca9557                expander;   ///< Active: routes B3 input + DG411 selects.

    Mcp4728                dac;
    daisy::DacHandle       stm_dac;
    daisy::SdmmcHandler    sdmmc;
    daisy::FatFSInterface  fatfs;

    /* ── Jacks (the new surface) ────────────────────────────────────────── */

    TriggerJack triggers[kNumTriggerJacks];   ///< J1, J2
    CvJack      cv_jacks[kNumCvJacks];        ///< J3..J10 (see header note)

    /* Named refs — match panel labels. Initialised via in-class init so
     * every constructor (and aggregate init) gets them for free. */
    TriggerJack& j1  = triggers[0];
    TriggerJack& j2  = triggers[1];
    CvJack&      j3  = cv_jacks[0];
    CvJack&      j4  = cv_jacks[1];
    CvJack&      j5  = cv_jacks[2];
    CvJack&      j6  = cv_jacks[3];
    CvJack&      j7  = cv_jacks[4];
    CvJack&      j8  = cv_jacks[5];
    CvJack&      j9  = cv_jacks[6];
    CvJack&      j10 = cv_jacks[7];

    /* Frozen-contract proxy — `hw.cv[i].Value()` reads the i-th switchable
     * CV jack (J3..J8). Existing framework + V1 firmware Just Works. */
    struct CvProxy
    {
        CvJack* slots[kNumCvInputs];
        CvJack&       operator[](uint8_t i)       { return *slots[i]; }
        const CvJack& operator[](uint8_t i) const { return *slots[i]; }
    };
    CvProxy cv {{ &cv_jacks[0], &cv_jacks[1], &cv_jacks[2],
                  &cv_jacks[3], &cv_jacks[4], &cv_jacks[5] }};

    /* ── Lifecycle ─────────────────────────────────────────────────────── */

    /**
     * Initialise hardware in dependency order:
     *   1.  Daisy Seed + audio params
     *   1b. Factory-cal hook — B1+B2 held → run cal, persist, reset
     *       (never returns); see v2_factory_cal.h
     *   1c. Load V2Calibration from QSPI (design fallback on failure)
     *   2.  ADC (pots + CV; CV channels get their per-jack calibration)
     *   3.  On-MCU buttons (B1, B2)
     *   4.  I²C1 @ 400 kHz
     *   5.  PCA9557 expander
     *   6.  MCP4728 quad DAC (seeds mid-scale, pulses LDAC via expander)
     *   7.  B3 via expander
     *   8.  WS2812 strip + LedPanel
     *   9.  STM32 internal DAC (DAC1 ch 1 + 2 seeded mid-scale)
     *   9b. Bind CvJack / TriggerJack instances to their backends
     *   10. SDMMC1 @ MEDIUM_SLOW / BITS_1
     *   11. FatFS lazy mount on volume "0:"
     */
    void Init(daisy::SaiHandle::Config::SampleRate sample_rate
                  = daisy::SaiHandle::Config::SampleRate::SAI_48KHZ,
              uint32_t block_size = kEngineBlockSamples);

    /**
     * Update all control state. Pots + CV via daisy::AnalogControl::Process,
     * on-MCU buttons via debounce, B3 via I²C-read + software debounce.
     * Call at 1 ms cadence.
     */
    void ProcessAllControls();

    /** Start audio with the given callback. Installs the trigger /
     *  codec-CV shim around it transparently. */
    void StartAudio(daisy::AudioHandle::AudioCallback cb);

    bool FlushCvOutputs();

    /* ── Accessors ─────────────────────────────────────────────────────── */

    const HardwareLayout& Layout() const { return kAlchemyLabV2Layout; }
    const ArcGeometry&    Arc()    const { return kAlchemyLabV2ArcGeometry; }
    float                 SampleRate() const { return sample_rate_hz_; }
    size_t                BlockSize()  const { return block_size_; }

    /** Peripheral health — non-aborting Init() records these so user
     *  firmware can guard before driving the corresponding hardware. */
    bool I2cReady()      const { return i2c_ready_; }
    bool ExpanderReady() const { return expander_ready_; }
    bool Mcp4728Ready()  const { return mcp4728_ready_; }
    bool StmDacReady()   const { return stm_dac_ready_; }

    /** True when a valid per-board cal record was loaded from QSPI.
     *  False means cv_jacks[i].Volts() / SetVolts() run on design-nominal
     *  constants (~3-5 % absolute error). */
    bool IsCalibrated() const { return cal_loaded_; }

    /** The active calibration record (measured or design fallback). */
    const V2Calibration& Calibration() const { return cal_; }

  private:
    float    sample_rate_hz_ = 0.0f;
    size_t   block_size_     = 0u;

    bool i2c_ready_      = false;
    bool expander_ready_ = false;
    bool mcp4728_ready_  = false;
    bool stm_dac_ready_  = false;

    bool          cal_loaded_ = false;
    V2Calibration cal_{};

    /** MCP4728 writes are all-four-channels; shadow the last value per
     *  channel so per-jack SetVolts doesn't disturb the others. */
    uint16_t mcp_shadow_[kMcp4728NumChannels] = { 2048u, 2048u, 2048u, 2048u };

    bool mcp_dirty_ = false;

    /* ── Audio shim plumbing ─────────────────────────────────────────── */

    daisy::AudioHandle::AudioCallback user_cb_ = nullptr;
    static AlchemyLabV2* s_instance_;
    static void AudioShim(daisy::AudioHandle::InputBuffer  in,
                          daisy::AudioHandle::OutputBuffer out,
                          size_t                           size);
};

} // namespace alchemy
