/**
 * @file alchemy_lab_v1_layout.h
 * @brief Alchemy Lab V1 BSP — pin assignments and hardware layout (data only).
 *
 * This header carries the raw hardware facts: pin maps, LED chain layout,
 * arc geometry, audio constants, QSPI flash region.  Include this directly
 * for the no-framework path; include "alchemy/hw/alchemy_lab_v1.h" to get
 * the AlchemyLabV1 board class (which includes this transitively).
*/

#pragma once

#include "daisy_seed.h"
#include "alchemy/hardware_types.h"
#include "alchemy/flash_ops.h"
#include "alchemy/hw/ws2812.h"

namespace alchemy {

constexpr uint16_t kEngineBlockSamples = 24;

constexpr uint8_t kNumPots = 6;

constexpr daisy::Pin kPinPot1 = daisy::seed::D17; /* C5  PB1  ADC2 */
constexpr daisy::Pin kPinPot2 = daisy::seed::D21; /* C6  PC4  ADC6 */
constexpr daisy::Pin kPinPot3 = daisy::seed::D18; /* C4  PA7  ADC3 */
constexpr daisy::Pin kPinPot4 = daisy::seed::D20; /* C3  PC1  ADC5 */
constexpr daisy::Pin kPinPot5 = daisy::seed::D16; /* C1  PA3  ADC1 */
constexpr daisy::Pin kPinPot6 = daisy::seed::D19; /* C2  PA6  ADC4 */

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
/*  CV Inputs (ADC, ±5 V hardware-scaled to 0–3.3 V)                        */
/* ========================================================================= */
/*
 * 0 V at jack (no signal) reads as ~0.5 normalised after hardware scaling.
 *
 *   J3/CV1 → C9  D22 (PA5  ADC7)    J4/CV2 → C8  D23 (PA4  ADC8)
 *   J5/CV3 → C10 D31 (PC2  ADC12)   J6/CV4 → C7  D15 (PC0  ADC0)
 *   J7/CV5 → D8  D32 (PC3  ADC13)   J8/CV6 → E1  D24 (PA1  ADC9)
 *
 */

constexpr uint8_t kNumCvInputs = 6;
constexpr uint8_t kCvAdcOffset = kNumPots;  /* CV channels start at index 6 */

constexpr daisy::Pin kCvPins[kNumCvInputs] = {
    daisy::seed::D22,  /* J3  C9  CV1 */
    daisy::seed::D23,  /* J4  C8  CV2 */
    daisy::seed::D31,  /* J5  C10 CV3 */
    daisy::seed::D15,  /* J6  C7  CV4 */
    daisy::seed::D32,  /* J7  D8  CV5 */
    daisy::seed::D24,  /* J8  E1  CV6 */
};

constexpr uint8_t kNumButtons = 3;

constexpr daisy::Pin kPinBtn1 = daisy::seed::D26; /* E3 PD11 */
constexpr daisy::Pin kPinBtn2 = daisy::seed::D0;  /* D9 PB12 */
constexpr daisy::Pin kPinBtn3 = daisy::seed::D2;  /* E9 PC10 */

constexpr daisy::Pin kBtnPins[kNumButtons] = {
    kPinBtn1, kPinBtn2, kPinBtn3
};

/**
 * Named indices into `AlchemyLabV1::buttons[]`.
 * Prefer these over magic integers in example/firmware code.
 */
constexpr uint8_t kButtonB1 = 0u;
constexpr uint8_t kButtonB2 = 1u;
constexpr uint8_t kButtonB3 = 2u;

/* ========================================================================= */
/*  WS2812 LED chain (102 LEDs)                                               */
/* ========================================================================= */
/*
 * Single-wire chain driven by TIM3_CH4 PWM + DMA on PC9 (E8 / D3).
 *
 * Chain order (transmission index → device):
 *   0..15   Pot 6 ring   CW from 9 o'clock
 *   16      B3 BOTTOM
 *   17..32  Pot 5 ring   CW from 3 o'clock
 *   33      B3 TOP
 *   34      B2 BOTTOM
 *   35..50  Pot 4 ring   CCW from 9 o'clock
 *   51      B2 TOP
 *   52..67  Pot 3 ring   CW from 2.25 h (1.5 steps CCW of 3 o'clock)
 *   68      B1 BOTTOM
 *   69..84  Pot 1 ring   CW from 3 o'clock
 *   85      B1 TOP
 *   86..101 Pot 2 ring   CW from 9.75 h (1 step CW of 9 o'clock)
 */

constexpr daisy::Pin kPinLedData = daisy::seed::D3; /* E8 PC9, TIM3_CH4 AF2 */

constexpr uint8_t  kLedsPerRing    = 16;
constexpr uint8_t  kLedsPerButton  = 2;
constexpr uint8_t  kNumLedRings    = kNumPots;
constexpr uint16_t kLedRingTotal   = kLedsPerRing   * kNumLedRings;   /* 96 */
constexpr uint16_t kLedButtonTotal = kLedsPerButton * kNumButtons;    /*  6 */
constexpr uint16_t kLedTotal       = kLedRingTotal + kLedButtonTotal; /* 102 */

constexpr float kLedHoursPerStep = 12.0f / static_cast<float>(kLedsPerRing); /* 0.75 */

/**
 * Standard arc geometry for Alchemy Lab V1 pot rings.
 *
 * Spans 7.5 h → 17.25 h (13 steps × 0.75 h) — the arc zone above the
 * 6 o'clock dead zone.  Pass this to animation functions and InitController
 * instead of repeating the three values at every call site.
 */
constexpr ArcGeometry kAlchemyLabV1ArcGeometry {
    7.5f,             /* start_hour: CCW arc endpoint */
    kLedHoursPerStep, /* step_hours: 0.75 h per LED  */
    13u,              /* arc_leds:   usable arc steps */
};


extern const HardwareLayout kAlchemyLabV1Layout;

/* ========================================================================= */
/*  QSPI preset flash region                                                  */
/* ========================================================================= */
/*
 * The IS25LP064A (8 MB, memory-mapped at 0x90000000) is shared between
 * firmware (low addresses) and user presets (high addresses).
 *
 * Preset region: 640 KiB at 0x90760000–0x907FFFFF
 *   16 slots × 2 sides × 5 sectors × 4 KiB = 640 KiB
 */

constexpr uint32_t kPresetFlashBase      = 0x90760000u; ///< Start of preset region
constexpr uint32_t kPresetSectorSize     = 4096u;        ///< IS25LP064A erase granule
constexpr uint8_t  kPresetSectorsPerSide = 5u;           ///< Sectors per ping-pong side
constexpr uint8_t  kPresetNumSlots       = 16u;          ///< Number of preset slots

constexpr float kCvZeroNorm = 0.5f;

inline float CvToBipolar(float cv_norm) { return cv_norm - kCvZeroNorm; }

FlashOps PresetFlashOps(daisy::QSPIHandle* qspi);

} // namespace alchemy
