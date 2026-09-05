/**
 * @file alchemy_lab_v2.cpp
 * @brief alchemy::AlchemyLabV2 — Init / ProcessAllControls / StartAudio /
 *        FlushCvOutputs.
 *
 * Production scope: Seed + audio, ADC (pots + CV), B1/B2 on-MCU buttons,
 * I²C1 + PCA9557 expander + B3, WS2812 + LedPanel, MCP4728 quad DAC,
 * STM32 internal DAC, SDMMC1 + FatFS. Jacks (J1..J10) bound via
 * CvJack / TriggerJack with a single audio shim around the user callback.
 */

#include "alchemy/hw/alchemy_lab_v2.h"
#include "alchemy/hw/v2_factory_cal.h"

#include "ff.h"

namespace alchemy {

AlchemyLabV2* AlchemyLabV2::s_instance_ = nullptr;

/* 20 Hz pot one-pole at the 1 kHz ProcessAllControls() cadence. */
static constexpr float kPotLpCoeff = 0.1181f; /* 1 - exp(-2π·20/1000) */

void AlchemyLabV2::Init(daisy::SaiHandle::Config::SampleRate sample_rate,
                        uint32_t                             block_size)
{
    /* 1) Seed bring-up + audio params ─────────────────────────────────── */
    seed.Configure();
    seed.Init();
    seed.SetAudioBlockSize(block_size);
    seed.SetAudioSampleRate(sample_rate);
    sample_rate_hz_ = seed.AudioSampleRate();
    block_size_     = seed.AudioBlockSize();

    /* 1b) Factory calibration hook — B1 + B2 held through boot. */
    if (V2FactoryCalRequested())
        V2RunFactoryCalibrationAndReset(*this);

    /* 1c) Load the per-board cal record from memory-mapped QSPI. */
    cal_loaded_ = V2CalLoadFromQspi(cal_);
    if (!cal_loaded_)
        V2CalDesignFallback(cal_);

    /* 2) ADC: 6 pot channels followed by 6 CV channels ─────────────────── */
    daisy::AdcChannelConfig adcCfg[kNumPots + kNumCvInputs];
    for (uint8_t i = 0; i < kNumPots; i++)
        adcCfg[i].InitSingle(kPotPins[i]);
    for (uint8_t i = 0; i < kNumCvInputs; i++)
        adcCfg[kCvAdcOffset + i].InitSingle(kCvPins[i]);
    seed.adc.Init(adcCfg, kNumPots + kNumCvInputs);
    seed.adc.Start();

    const float sr = seed.AudioCallbackRate();
    for (uint8_t i = 0; i < kNumPots; i++)
        pots[i].Init(seed.adc.GetPtr(i), sr, kPotPolarityFlipped, false, 0.0f);

    /* Init's slew arg is calibrated to the audio-callback rate, so set
     * the pot coeff directly; prime through the slew-0 pass-through so
     * val_ starts on-position instead of converging from 0. */
    daisy::System::Delay(2); /* first ADC scan */
    for (uint8_t i = 0; i < kNumPots; i++)
    {
        pots[i].Process();
        pots[i].SetCoeff(kPotLpCoeff);
    }

    /* 3) On-MCU buttons (B1, B2) ──────────────────────────────────────── */
    for (uint8_t i = 0; i < kNumOnMcuButtons; i++)
        on_mcu_buttons[i].Init(kOnMcuBtnPins[i]);

    /* 4) I²C1 @ 400 kHz ──────────────────────────────────────────────── */
    daisy::I2CHandle::Config i2c_cfg;
    i2c_cfg.periph          = daisy::I2CHandle::Config::Peripheral::I2C_1;
    i2c_cfg.speed           = daisy::I2CHandle::Config::Speed::I2C_400KHZ;
    i2c_cfg.mode            = daisy::I2CHandle::Config::Mode::I2C_MASTER;
    i2c_cfg.pin_config.scl  = daisy::Pin(daisy::PORTB, 8);
    i2c_cfg.pin_config.sda  = daisy::Pin(daisy::PORTB, 9);
    i2c_ready_ = (i2c.Init(i2c_cfg) == daisy::I2CHandle::Result::OK);

    /* 5) PCA9557 expander ─────────────────────────────────────────────── */
    if (i2c_ready_)
        expander_ready_ = expander.Init(i2c, kPca9557Address);

    /* 6) MCP4728 quad DAC */
    if (i2c_ready_)
    {
        mcp4728_ready_ = dac.Init(i2c, kMcp4728AddrFirst, kMcp4728AddrLast);
        if (mcp4728_ready_ && expander_ready_)
            dac.PulseLdac(expander, kPca9557IoLdac);
    }

    /* 7) B3 via expander ──────────────────────────────────────────────── */
    if (expander_ready_)
        b3.Init(expander, kPca9557IoB3);

    /* 8) WS2812 strip + LedPanel ──────────────────────────────────────── */
    strip.Init(kLedTotal);
    leds.Init(strip, kAlchemyLabV2Layout);

    /* 9) STM32 internal DAC (DAC1 ch 1 + 2). */
    {
        daisy::DacHandle::Config dac_cfg;
        dac_cfg.target_samplerate = 48000u;
        dac_cfg.chn               = daisy::DacHandle::Channel::BOTH;
        dac_cfg.mode              = daisy::DacHandle::Mode::POLLING;
        dac_cfg.bitdepth          = daisy::DacHandle::BitDepth::BITS_12;
        dac_cfg.buff_state        = daisy::DacHandle::BufferState::ENABLED;
        stm_dac_ready_ =
            (stm_dac.Init(dac_cfg) == daisy::DacHandle::Result::OK);
        if (stm_dac_ready_)
        {
            stm_dac.WriteValue(daisy::DacHandle::Channel::ONE, 2048u);
            stm_dac.WriteValue(daisy::DacHandle::Channel::TWO, 2048u);
        }
    }

    /* 9b) Bind CvJack / TriggerJack to their backends.
     *
     * cv_jacks[0..3] → J3..J6 MCP4728 channels A..D, all sharing the
     * mcp_shadow_ array. cv_jacks[4..5] → J7, J8 STM DAC1 ch1, ch2.
     * cv_jacks[6..7] → J9, J10 codec output channels. Triggers bind to
     * codec input channels by index — actual sample-block processing
     * happens inside the audio shim. */

    /* Input-side voltage reference. The record's VDDA is what the zero
     * codes and the DAC fits were measured against (v2_factory_cal.cpp
     * pass 1/3), so Volts() must use it too. Guard the persisted float:
     * a CRC-valid but implausible value (bench-written record) must not
     * poison every read — !(lo ≤ x ≤ hi) also rejects NaN. Window
     * matches factory cal's own VREFINT validation. */
    float cal_vdda = cal_.vdda_at_cal;
    if (!(cal_vdda >= 3.0f && cal_vdda <= 3.6f))
        cal_vdda = kV2VddaDesign;

    for (uint8_t i = 0; i < kNumCvInputs; i++)
    {
        const DacRoute& route = kDacRouting[i];
        const uint8_t   src   = static_cast<uint8_t>(route.source);
        const V2JackCal* cal  = &cal_.jack[i];

        if (src < kMcp4728NumChannels)
        {
            cv_jacks[i].InitMcp(
                seed.adc.GetPtr(kCvAdcOffset + i), sr, cal, cal_vdda,
                &dac, src,
                &mcp_shadow_[src], mcp_shadow_, &mcp_dirty_,
                &expander, route.select_io, kPca9557IoLdac);
        }
        else
        {
            const auto stm_ch = (src == 4u)
                ? daisy::DacHandle::Channel::ONE
                : daisy::DacHandle::Channel::TWO;
            cv_jacks[i].InitStm(
                seed.adc.GetPtr(kCvAdcOffset + i), sr, cal, cal_vdda,
                &stm_dac, stm_ch,
                &expander, route.select_io);
        }
    }
    cv_jacks[kNumCvInputs + 0].InitCodec(kCodecOutChJ9);   /* J9  */
    cv_jacks[kNumCvInputs + 1].InitCodec(kCodecOutChJ10);  /* J10 */
    /* TriggerJacks need no per-instance binding — channel indexing is
     * implicit (triggers[0] ↔ in[0], triggers[1] ↔ in[1]) in AudioShim. */

    /* 10) SDMMC1 @ MEDIUM_SLOW / BITS_1 */
    {
        daisy::SdmmcHandler::Config sd_cfg;
        sd_cfg.Defaults();
        sd_cfg.speed = daisy::SdmmcHandler::Speed::MEDIUM_SLOW;
        sd_cfg.width = daisy::SdmmcHandler::BusWidth::BITS_1;
        const bool sdmmc_ready =
            (sdmmc.Init(sd_cfg) == daisy::SdmmcHandler::Result::OK);
        (void)sdmmc_ready;
    }

    /* 11) FatFs lazy mount on volume "0:". */
    {
        daisy::FatFSInterface::Config ff_cfg;
        ff_cfg.media = daisy::FatFSInterface::Config::MEDIA_SD;
        if (fatfs.Init(ff_cfg) == daisy::FatFSInterface::OK)
        {
            (void)f_mount(&fatfs.GetSDFileSystem(), fatfs.GetSDPath(), 0);
        }
    }
}

bool AlchemyLabV2::FlushCvOutputs()
{
    if (!mcp_dirty_)
        return true;
    if (!mcp4728_ready_ || !expander_ready_)
        return false;

    if (!dac.WriteAll(mcp_shadow_[0], mcp_shadow_[1], mcp_shadow_[2], mcp_shadow_[3]))
        return false;
        
    if (!dac.PulseLdac(expander, kPca9557IoLdac))
        return false;

    mcp_dirty_ = false;
    return true;
}

void AlchemyLabV2::ProcessAllControls()
{
    for (uint8_t i = 0; i < kNumPots; i++)         pots[i].Process();
    /* Only the analog cv jacks (J3..J8) have an ADC — codec jacks J9/J10
     * are output-only and CvJack::Process() short-circuits there. */
    for (uint8_t i = 0; i < kNumCvInputs; i++)     cv_jacks[i].Process();
    for (uint8_t i = 0; i < kNumOnMcuButtons; i++) on_mcu_buttons[i].Poll();
    b3.Poll();
}

void AlchemyLabV2::StartAudio(daisy::AudioHandle::AudioCallback cb)
{
    user_cb_    = cb;
    s_instance_ = this;
    seed.StartAudio(&AlchemyLabV2::AudioShim);
}

void AlchemyLabV2::AudioShim(daisy::AudioHandle::InputBuffer  in,
                             daisy::AudioHandle::OutputBuffer out,
                             size_t                           size)
{
    AlchemyLabV2* hw = s_instance_;
    if (!hw) return;

    /* Pre: feed J1/J2 sample blocks to trigger detectors. Read-only on
     * in[] — never alters the user callback's view of audio input. */
    hw->triggers[0].ProcessBlock(in[kCodecInChJ1], size);
    hw->triggers[1].ProcessBlock(in[kCodecInChJ2], size);

    /* User audio callback runs as written. */
    if (hw->user_cb_) hw->user_cb_(in, out, size);

    /* Post: for any codec jack claimed for CV out (EnableCvOutput), fill
     * its channel with the staged target value. Whatever the user wrote
     * to that channel is silently replaced — the README documents this. */
    for (uint8_t k = 0; k < kNumCodecCvOuts; ++k)
    {
        CvJack& j = hw->cv_jacks[kNumCvInputs + k];
        if (!j.connected_) continue;
        const float sample = j.target_v_ / kCodecJackFullScaleVolts;
        const uint8_t ch = j.codec_channel_;
        for (size_t i = 0; i < size; ++i) out[ch][i] = sample;
    }
}

} // namespace alchemy
