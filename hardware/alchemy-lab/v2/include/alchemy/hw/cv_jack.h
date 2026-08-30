/**
 * @file cv_jack.h
 * @brief alchemy::CvJack — unified CV-capable jack for J3..J10.
 *
 * One C++ type covers three backends — same six public methods regardless
 * of which one is wired beneath:
 *
 *   J3..J6   MCP4728 quad I²C DAC, via DG411 routing switch
 *   J7, J8   STM32 DAC1 ch1/ch2 (memory-mapped), via DG411 routing switch
 *   J9, J10  Audio codec output, DC-coupled — "Enable" claims the channel
 *            from the audio callback; SDK fills the buffer per block.
 *
 * See the jack reference table in the SDK README for per-jack precision,
 * latency, and notes. End-user code never branches on backend kind.
 *
 *   hw.j3.EnableCvOutput();      // close DG411
 *   hw.j3.SetVolts(2.5f);        // ±5 V at the panel
 *   float v = hw.j6.Volts();     // ADC, calibrated (input mode)
 *
 *   hw.j9.EnableCvOutput();      // claim codec out channel
 *   hw.j9.SetVolts(precise);     // 24-bit DC, audio-block latency
 *
 * Driving several MCP jacks in one control tick: stage each, latch once.
 *
 *   hw.j3.StageVolts(a);
 *   hw.j4.StageVolts(b);
 *   hw.FlushCvOutputs();         // one WriteAll + one LDAC pulse
 */

#pragma once

#include <cstdint>
#include "daisy_seed.h"
#include "per/dac.h"

#include "alchemy/hw/cv_input.h"
#include "alchemy/hw/v2_calibration.h"

namespace alchemy {

class AlchemyLabV2;  // friend for Init* + audio shim access
class Mcp4728;
class Pca9557;

class CvJack
{
  public:
    /* ── Read ─────────────────────────────────────────────────────────── */

    /** Calibrated jack voltage.
     *  J3..J8: input ADC reading. Lab-accurate (~±10 mV) in input mode;
     *          approximate (~±50 mV) in output mode (cal was measured
     *          DG411-open).
     *  J9, J10: no ADC readback — returns the last SetVolts target. */
    float Volts() const;

    /** Raw normalised 0..1 value (matches the V1 / V2 input semantics
     *  for J3..J8; for J9/J10 returns the staged target scaled to 0..1). */
    float Value() const;

    /* ── Write ────────────────────────────────────────────────────────── */

    /** Drive the jack to `volts` (±5 V). Reaches the panel only when
     *  EnableCvOutput() is in effect — until then the DAC code is
     *  staged but the jack is disconnected. Returns false on backend
     *  unavailable. */
    bool SetVolts(float volts);

    /** Stage `volts` without latching. J3..J6 write the shared MCP shadow,
     *  applied on the next AlchemyLabV2::FlushCvOutputs(); J7..J10 have
     *  nothing to batch, so this is exactly SetVolts. Returns false on
     *  backend unavailable. */
    bool StageVolts(float volts);

    /* ── Direction ───────────────────────────────────────────────────── */

    /** Put this jack into CV-output mode.
     *  J3..J8 — closes the DG411 routing switch.
     *  J9, J10 — claims the codec channel from the audio callback. */
    bool EnableCvOutput();

    /** Return to the default state.
     *  J3..J8 — opens the DG411; jack is a CV input.
     *  J9, J10 — releases the codec channel back to the audio callback. */
    bool DisableCvOutput();

    bool IsCvOutput() const { return connected_; }

  private:
    friend class AlchemyLabV2;

    enum class Backend : uint8_t { Mcp, Stm, CodecOut };

    /** MCP4728 jack (J3..J6). `shadow_slot` points into the shared
     *  4-channel shadow array and `dirty_flag` at the board's staged-write
     *  flag — both owned by AlchemyLabV2, which outlives every jack.
     *  `vdda` is the board VDDA out of the cal record — the reference the
     *  record's zero codes and DAC fits were measured against
     *  (kV2VddaDesign when uncalibrated). */
    void InitMcp(uint16_t*           adc_ptr,
                 float               sample_rate,
                 const V2JackCal*    cal,
                 float               vdda,
                 Mcp4728*            mcp,
                 uint8_t             mcp_channel,
                 uint16_t*           shadow_slot,
                 uint16_t*           shadow_array,
                 bool*               dirty_flag,
                 Pca9557*            expander,
                 uint8_t             select_io,
                 uint8_t             ldac_io);

    /** STM DAC1 jack (J7, J8). `vdda` as in InitMcp. */
    void InitStm(uint16_t*                 adc_ptr,
                 float                     sample_rate,
                 const V2JackCal*          cal,
                 float                     vdda,
                 daisy::DacHandle*         stm,
                 daisy::DacHandle::Channel stm_ch,
                 Pca9557*                  expander,
                 uint8_t                   select_io);

    /** Codec output jack (J9, J10). `codec_channel` is the index into the
     *  audio callback's out[] buffer (0 = left, 1 = right). */
    void InitCodec(uint8_t codec_channel);

    /** Called by AlchemyLabV2::ProcessAllControls() — updates the input
     *  AnalogControl for jacks that have one. Cheap no-op for codec out. */
    void Process();

    uint16_t VoltsToCode(float volts) const;

    /* ── Common state ─────────────────────────────────────────────────── */

    Backend          backend_   = Backend::Mcp;
    CvInput          in_;                 /* unused for CodecOut          */
    const V2JackCal* cal_       = nullptr;
    Pca9557*         expander_  = nullptr;  /* null for CodecOut          */
    uint8_t          select_io_ = 0;        /* unused for CodecOut        */
    bool             connected_ = false;    /* shadow of routing state    */
    float            target_v_  = 0.0f;     /* last successful SetVolts   */

    /* ── MCP4728-only ─────────────────────────────────────────────────── */

    Mcp4728*  mcp_              = nullptr;
    uint8_t   mcp_channel_      = 0;
    uint16_t* mcp_shadow_slot_  = nullptr;   /* points into shadow_array */
    uint16_t* mcp_shadow_array_ = nullptr;   /* whole 4-channel shadow   */
    bool*     mcp_dirty_        = nullptr;   /* board's staged-write flag */
    uint8_t   ldac_io_          = 0;

    /* ── STM DAC-only ─────────────────────────────────────────────────── */

    daisy::DacHandle*         stm_    = nullptr;
    daisy::DacHandle::Channel stm_ch_ = daisy::DacHandle::Channel::ONE;

    /* ── Codec-only ───────────────────────────────────────────────────── */

    uint8_t codec_channel_ = 0;
};

} // namespace alchemy
