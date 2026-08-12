/**
 * @file alchemy_lab_v2_layout.h
 * @brief Alchemy Lab V2 BSP — pin assignments and hardware layout (data only).
 *
 * Source of truth for V2 hardware:
 *  - Pin tables (pots, CV, on-MCU buttons, LED data, STM32 DAC)
 *  - I²C device addresses and PCA9557 pinout
 *  - DG411 polarity and DAC→jack routing
 *  - LED chain layout / arc geometry / QSPI preset region
 *
 */

#pragma once

#include "daisy_seed.h"
#include "alchemy/hardware_types.h"
#include "alchemy/flash_ops.h"

namespace alchemy {

constexpr uint16_t kEngineBlockSamples = 24;

/* ========================================================================= */
/*  Pots (6 channels, ADC)                                                   */
/* ========================================================================= */

constexpr uint8_t kNumPots = 6;

constexpr daisy::Pin kPinPot1 = daisy::seed::D31; /* A12 PC2 */
constexpr daisy::Pin kPinPot2 = daisy::seed::D24; /* A9  PA1 */
constexpr daisy::Pin kPinPot3 = daisy::seed::D32; /* A13 PC3 */
constexpr daisy::Pin kPinPot4 = daisy::seed::D28; /* A11 PA2 */
constexpr daisy::Pin kPinPot5 = daisy::seed::D15; /* A0  PC0 */
constexpr daisy::Pin kPinPot6 = daisy::seed::D25; /* A10 PA0 */

constexpr daisy::Pin kPotPins[kNumPots] = {
    kPinPot1, kPinPot2, kPinPot3, kPinPot4, kPinPot5, kPinPot6
};

constexpr bool kPotPolarityFlipped = true;

/*
 * Pot indices by panel position (front view): two columns of three,
 * odd-numbered pots down the left, even down the right, B1/B2/B3
 * beside the top/middle/bottom rows.
 *
 *      [P1] B1 [P2]
 *      [P3] B2 [P4]
 *      [P5] B3 [P6]
 */
constexpr uint8_t kPotTopLeft     = 0u;
constexpr uint8_t kPotTopRight    = 1u;
constexpr uint8_t kPotMiddleLeft  = 2u;
constexpr uint8_t kPotMiddleRight = 3u;
constexpr uint8_t kPotBottomLeft  = 4u;
constexpr uint8_t kPotBottomRight = 5u;

/* ========================================================================= */
/*  CV inputs (6 jacks, ADC1, ±5 V hardware-scaled to 0–3.3 V)                */
/* ========================================================================= */
/*
 * Inverting summer (MCP6004 on +3V3A) per channel:
 *   V_pin = 1.65 V − 0.165 × V_jack
 * High ADC code = negative jack voltage. The 1.65 V bias is set by the
 * −10 V reference + R ratio, NOT by VDDA/2. Volts come from the
 * calibrated path (cv[i].Volts() — see v2_calibration.h).
 *
 * Per-jack ADC mapping:
 *
 *   Jack | Daisy pin | MCU pin | ADC1 channel
 *   -----+-----------+---------+-------------
 *   J3   | D19       | PA6     | INP3
 *   J4   | D16       | PA3     | INP15
 *   J5   | D21       | PC4     | INP4
 *   J6   | D17       | PB1     | INP5
 *   J7   | D18       | PA7     | INP7
 *   J8   | D20       | PC1     | INP11
 */

constexpr uint8_t kNumCvInputs = 6;
constexpr uint8_t kCvAdcOffset = kNumPots;  /* CV channels start at index 6 */

constexpr daisy::Pin kCvPins[kNumCvInputs] = {
    daisy::seed::D19, /* J3  PA6   ADC1_INP3  */
    daisy::seed::D16, /* J4  PA3   ADC1_INP15 */
    daisy::seed::D21, /* J5  PC4   ADC1_INP4  */
    daisy::seed::D17, /* J6  PB1   ADC1_INP5  */
    daisy::seed::D18, /* J7  PA7   ADC1_INP7  */
    daisy::seed::D20, /* J8  PC1   ADC1_INP11 */
};

/** Raw ADC1 channel numbers, parallel to kCvPins. Useful for bare-metal
 *  ADC paths (e.g., direct PCSEL writes during calibration) that bypass
 *  Daisy's pin abstractions. Also just documenting it, because. */
constexpr uint32_t kCvAdc1Channels[kNumCvInputs] = {
    3u,   /* J3  PA6 */
    15u,  /* J4  PA3 */
    4u,   /* J5  PC4 */
    5u,   /* J6  PB1 */
    7u,   /* J7  PA7 */
    11u,  /* J8  PC1 */
};

/* ========================================================================= */
/*  Buttons                                                                   */
/* ========================================================================= */
/*
 * B1 and B2 are on-MCU GPIO.
 * B3 is off-MCU: read via PCA9557 IO6 (see Pca9557Button).
 */

constexpr uint8_t kNumOnMcuButtons = 2;
constexpr uint8_t kNumButtons      = 3;

constexpr daisy::Pin kPinBtn1 = daisy::seed::D0;  /* PB12 */
constexpr daisy::Pin kPinBtn2 = daisy::seed::D27; /* PG9  */

constexpr daisy::Pin kOnMcuBtnPins[kNumOnMcuButtons] = {
    kPinBtn1, kPinBtn2
};

constexpr uint8_t kButtonB1 = 0u;
constexpr uint8_t kButtonB2 = 1u;
constexpr uint8_t kButtonB3 = 2u;

constexpr uint8_t kNumStmDacs = 2;

/* ========================================================================= */
/*  WS2812 LED chain (102 LEDs, data on D26 / PD11)                          */
/* ========================================================================= */
/*
 * V2 LED data pin has no TIM channel — see src/ws2812.cpp for the
 * TIM6-DMA-BSRR driver.
 *
 *   0..15   Pot 6 ring   CW from 9 o'clock
 *   16      B3 BOTTOM
 *   17..32  Pot 5 ring   CW from 3 o'clock
 *   33      B3 TOP
 *   34      B2 BOTTOM
 *   35..50  Pot 4 ring   CCW from 9 o'clock
 *   51      B2 TOP
 *   52..67  Pot 3 ring   CW from 2.25 h
 *   68      B1 BOTTOM
 *   69..84  Pot 1 ring   CW from 3 o'clock
 *   85      B1 TOP
 *   86..101 Pot 2 ring   CW from 9.75 h
 */

constexpr daisy::Pin kPinLedData = daisy::seed::D26; /* PD11 */

constexpr uint8_t  kLedsPerRing    = 16;
constexpr uint8_t  kLedsPerButton  = 2;
constexpr uint8_t  kNumLedRings    = kNumPots;
constexpr uint16_t kLedRingTotal   = kLedsPerRing   * kNumLedRings;   /* 96 */
constexpr uint16_t kLedButtonTotal = kLedsPerButton * kNumButtons;    /*  6 */
constexpr uint16_t kLedTotal       = kLedRingTotal + kLedButtonTotal; /* 102 */

constexpr float kLedHoursPerStep = 12.0f / static_cast<float>(kLedsPerRing); /* 0.75 */

constexpr ArcGeometry kAlchemyLabV2ArcGeometry {
    7.5f,             /* start_hour: CCW arc endpoint */
    kLedHoursPerStep, /* step_hours: 0.75 h per LED  */
    13u,              /* arc_leds:   usable arc steps */
};

/* ========================================================================= */
/*  I²C1                                                                      */
/* ========================================================================= */
/*
 * SCL = PB8, SDA = PB9 (Daisy Seed default I²C1 pins).
 * Speed: 400 kHz (fast mode).
 */

/* ========================================================================= */
/*  PCA9557PW GPIO expander                                                   */
/* ========================================================================= */
/*
 * All ADDR pins to GND → 7-bit address 0x18.
 *
 * IO usage (verified Phase 1):
 *   IO0..IO5  outputs — DG411 select lines (REVERSED jack order:
 *                       IO0 drives J8, IO5 drives J3 — see kDacRouting)
 *   IO6       input   — B3 button
 *   IO7       output  — LDAC for MCP4728 (active LOW)
 *
 * Boot register state:
 *   Configuration (0x03) = 0x40   IO6 input, others output
 *   Polarity      (0x02) = 0x00
 *   Output port   (0x01) = 0xFF   selects HIGH (DG411 OFF), LDAC HIGH (inactive)
 */

constexpr uint8_t kPca9557Address      = 0x18u;
constexpr uint8_t kPca9557RegInput     = 0x00u;
constexpr uint8_t kPca9557RegOutput    = 0x01u;
constexpr uint8_t kPca9557RegPolarity  = 0x02u;
constexpr uint8_t kPca9557RegConfig    = 0x03u;

constexpr uint8_t kPca9557ConfigBoot   = 0x40u; /* IO6 input, all others output */
constexpr uint8_t kPca9557PolarityBoot = 0x00u;
constexpr uint8_t kPca9557OutputBoot   = 0xFFu;

constexpr uint8_t kPca9557IoB3   = 6u;  /* IO6: B3 button input  */
constexpr uint8_t kPca9557IoLdac = 7u;  /* IO7: LDAC, active LOW */

/* ========================================================================= */
/*  DG411 polarity                                                            */
/* ========================================================================= */
/*
 * DG411: switch CLOSES on logic LOW at the control input.
 * Parameterised so a future DG412 swap is a one-line change.
 *
 *   kDg411OnLevel = false  → write 0 to the select bit to route DAC to jack
 *                          → write 1 to disconnect (default state at boot)
 */

constexpr bool kDg411OnLevel = false;

/* ========================================================================= */
/*  DAC → jack routing                                                        */
/* ========================================================================= */
/*
 *
 * Inverting summer (TL07x on ±12 V) per channel:
 *   V_jack = 5 V − 3.03 × V_dac_pin
 * → DAC code 0 produces +5 V at the jack; code 4095 produces −5 V.
 *
 *   Jack | DAC source              | PCA9557 IO (DG411 select)
 *   -----+-------------------------+---------------------------
 *   J3   | MCP4728 VOUTA           | IO5
 *   J4   | MCP4728 VOUTB           | IO4
 *   J5   | MCP4728 VOUTC           | IO3
 *   J6   | MCP4728 VOUTD           | IO2
 *   J7   | STMDAC1 (DAC1 ch1, PA4) | IO1
 *   J8   | STMDAC2 (DAC1 ch2, PA5) | IO0
 *
 */

enum class DacSource : uint8_t {
    Mcp4728A = 0,
    Mcp4728B = 1,
    Mcp4728C = 2,
    Mcp4728D = 3,
    StmDac1  = 4,
    StmDac2  = 5,
};

constexpr uint8_t kNumDacOuts = 6;

struct DacRoute {
    DacSource source;     ///< Which DAC supplies this jack.
    uint8_t   select_io;  ///< PCA9557 IO bit that closes the DG411 switch.
};

constexpr DacRoute kDacRouting[kNumDacOuts] = {
    /* J3 */ { DacSource::Mcp4728A, 5u },
    /* J4 */ { DacSource::Mcp4728B, 4u },
    /* J5 */ { DacSource::Mcp4728C, 3u },
    /* J6 */ { DacSource::Mcp4728D, 2u },
    /* J7 */ { DacSource::StmDac1,  1u },
    /* J8 */ { DacSource::StmDac2,  0u },
};

/* ========================================================================= */
/*  MCP4728 quad DAC                                                          */
/* ========================================================================= */
/*
 * Factory-default address depends on the part suffix (A0..A7 → 0x60..0x67).
 * Probed at boot; first ACK wins.
 *
 * Boot config (per channel):
 *   VREF = VDD (3.3 V), Gain = 1, PD = normal.
 *   Output swing 0..3.3 V at the MCP, scaled by panel network onward to the jack.
 */

constexpr uint8_t kMcp4728AddrFirst = 0x60u;
constexpr uint8_t kMcp4728AddrLast  = 0x67u;
constexpr uint8_t kMcp4728NumChannels = 4u;

/* ========================================================================= */
/*  Codec jack mapping                                                        */
/* ========================================================================= */
/*
 * J1, J2  — codec inputs (AC-coupled audio in), routed as trigger jacks.
 * J9, J10 — codec outputs (DC-coupled audio out), routed as CV-out jacks.
 *
 * Channel indices match the audio callback's in[]/out[] buffer layout:
 *   J1 ↔ in[0]   J9  ↔ out[0]
 *   J2 ↔ in[1]   J10 ↔ out[1]
 *
 * The standard Daisy codec analog stages map ±5 V at the panel ↔ ±1.0 in
 * normalised sample units. Used by the audio shim to convert SetVolts()
 * targets to codec samples and to threshold trigger inputs.
 */
constexpr uint8_t kNumTriggerJacks = 2u;  /* J1, J2 */
constexpr uint8_t kNumCodecCvOuts  = 2u;  /* J9, J10 */
constexpr uint8_t kNumCvJacks      = kNumCvInputs + kNumCodecCvOuts; /* 8 */

constexpr uint8_t kCodecInChJ1  = 0u;
constexpr uint8_t kCodecInChJ2  = 1u;
constexpr uint8_t kCodecOutChJ9  = 0u;
constexpr uint8_t kCodecOutChJ10 = 1u;

constexpr float kCodecJackFullScaleVolts = 5.0f;

extern const HardwareLayout kAlchemyLabV2Layout;

constexpr uint32_t kPresetFlashBase      = 0x90760000u;
constexpr uint32_t kPresetSectorSize     = 4096u;
constexpr uint8_t  kPresetSectorsPerSide = 5u;
constexpr uint8_t  kPresetNumSlots       = 16u;

FlashOps PresetFlashOps(daisy::QSPIHandle* qspi);

} // namespace alchemy
