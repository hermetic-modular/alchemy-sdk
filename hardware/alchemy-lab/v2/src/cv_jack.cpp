/**
 * @file cv_jack.cpp
 * @brief CvJack — backend-dispatched read/write/route for J3..J10.
 */

#include "alchemy/hw/cv_jack.h"
#include "alchemy/hw/alchemy_lab_v2_layout.h"   /* kDg411OnLevel, etc.   */
#include "alchemy/hw/mcp4728.h"
#include "alchemy/hw/pca9557.h"

namespace alchemy {

namespace {
constexpr float kCodecFullScaleVolts = 5.0f;
}

/* ── Init paths ─────────────────────────────────────────────────────── */

void CvJack::InitMcp(uint16_t*        adc_ptr,
                     float            sample_rate,
                     const V2JackCal* cal,
                     float            vdda,
                     Mcp4728*         mcp,
                     uint8_t          mcp_channel,
                     uint16_t*        shadow_slot,
                     uint16_t*        shadow_array,
                     Pca9557*         expander,
                     uint8_t          select_io,
                     uint8_t          ldac_io)
{
    backend_          = Backend::Mcp;
    in_.Init(adc_ptr, sample_rate, /*flip=*/true, /*invert=*/false, /*slew=*/0.0f);
    cal_              = cal;
    expander_         = expander;
    select_io_        = select_io;
    connected_        = false;
    target_v_         = 0.0f;
    mcp_              = mcp;
    mcp_channel_      = mcp_channel;
    mcp_shadow_slot_  = shadow_slot;
    mcp_shadow_array_ = shadow_array;
    ldac_io_          = ldac_io;
    if (cal_) in_.SetCalibration(cal_->adc_zero_code, vdda);
}

void CvJack::InitStm(uint16_t*                 adc_ptr,
                     float                     sample_rate,
                     const V2JackCal*          cal,
                     float                     vdda,
                     daisy::DacHandle*         stm,
                     daisy::DacHandle::Channel stm_ch,
                     Pca9557*                  expander,
                     uint8_t                   select_io)
{
    backend_   = Backend::Stm;
    in_.Init(adc_ptr, sample_rate, /*flip=*/true, /*invert=*/false, /*slew=*/0.0f);
    cal_       = cal;
    expander_  = expander;
    select_io_ = select_io;
    connected_ = false;
    target_v_  = 0.0f;
    stm_       = stm;
    stm_ch_    = stm_ch;
    if (cal_) in_.SetCalibration(cal_->adc_zero_code, vdda);
}

void CvJack::InitCodec(uint8_t codec_channel)
{
    backend_       = Backend::CodecOut;
    cal_           = nullptr;
    expander_      = nullptr;
    select_io_     = 0;
    connected_     = false;
    target_v_      = 0.0f;
    codec_channel_ = codec_channel;
}

void CvJack::Process()
{
    if (backend_ != Backend::CodecOut)
        in_.Process();
}

/* ── Read ───────────────────────────────────────────────────────────── */

float CvJack::Volts() const
{
    if (backend_ == Backend::CodecOut)
        return target_v_;
    return in_.Volts();
}

float CvJack::Value() const
{
    if (backend_ == Backend::CodecOut)
    {
        /* Map target ±5 V to 0..1, matching the V1/V2 CvInput convention
         * where higher = more positive jack voltage. */
        float v = target_v_ / (2.0f * kCodecFullScaleVolts) + 0.5f;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        return v;
    }
    return in_.Value();
}

/* ── Write ──────────────────────────────────────────────────────────── */

bool CvJack::SetVolts(float volts)
{
    target_v_ = volts;

    if (backend_ == Backend::CodecOut)
    {
        /* No driver to poke — the audio shim reads target_v_ each block. */
        return true;
    }

    if (!cal_) return false;

    /* Invert the per-jack linear fit, clamp to the 12-bit hardware range.
       Past the linear region (cal_->dac_{min,max}_linear_code) the output
       compresses near the DAC's supply rails, so the jack stops tracking
       the fit — but pushing to 0/4095 still gets us as close to ±5 V as
       the analog stage physically allows, which matters more than
       perfect linearity in the last ~100 mV. */
    float code_f = (volts - cal_->dac_offset_v) / cal_->dac_gain_v_per_code;
    if (code_f < 0.0f)      code_f = 0.0f;
    if (code_f > 4095.0f)   code_f = 4095.0f;
    const uint16_t code = static_cast<uint16_t>(code_f + 0.5f);

    if (backend_ == Backend::Mcp)
    {
        if (!mcp_ || !mcp_->Ready() || !expander_ || !mcp_shadow_slot_
            || !mcp_shadow_array_)
            return false;
        *mcp_shadow_slot_ = code;
        if (!mcp_->WriteAll(mcp_shadow_array_[0], mcp_shadow_array_[1],
                            mcp_shadow_array_[2], mcp_shadow_array_[3]))
            return false;
        return mcp_->PulseLdac(*expander_, ldac_io_);
    }

    /* STM DAC */
    if (!stm_) return false;
    return stm_->WriteValue(stm_ch_, code) == daisy::DacHandle::Result::OK;
}

/* ── Direction ──────────────────────────────────────────────────────── */

bool CvJack::EnableCvOutput()
{
    if (backend_ == Backend::CodecOut)
    {
        connected_ = true;
        return true;
    }
    if (!expander_) return false;
    if (!expander_->SetOutputBit(select_io_, kDg411OnLevel))
        return false;
    connected_ = true;
    return true;
}

bool CvJack::DisableCvOutput()
{
    if (backend_ == Backend::CodecOut)
    {
        connected_ = false;
        return true;
    }
    if (!expander_) return false;
    if (!expander_->SetOutputBit(select_io_, !kDg411OnLevel))
        return false;
    connected_ = false;
    return true;
}

} // namespace alchemy
