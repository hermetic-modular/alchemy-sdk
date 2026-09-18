/**
 * @file v2_factory_cal.h
 * @brief Alchemy Lab V2 — on-board factory calibration (SDK-resident).
 *
 * The full self-cal procedure (VREFINT → VDDA, per-jack ADC zero, per-jack
 * DAC loopback sweep with rail-saturation detection) runs on the board
 * itself with nothing patched in, persists a V2Calibration record to the
 * dedicated QSPI sector, then soft-resets into a normal boot.
 *
 * Entry: hold B1 + B2 while the board powers up / resets. The check and
 * the procedure run at the very top of AlchemyLabV2::Init() — before the
 * DMA ADC starts — so the cal's one-shot bare-metal ADC use can't collide
 * with the streaming pot/CV path.
 *
 * Operator feedback is on the 102-LED panel (no USB dependency). Ring k
 * mirrors jack k (ring 0 ↔ J3 … ring 5 ↔ J8):
 *   - B1+B2 pairs white      : button hold acknowledged, cal starting
 *   - button pairs amber     : VREFINT / VDDA measurement
 *   - rings fill blue (wave) : per-jack ADC zero pass
 *   - ring fills amber       : that jack's DAC sweep in progress
 *     (finished jacks hold dim green)
 *   - triple green flash,
 *     then solid green       : success — release the buttons; the board
 *                              resets into normal boot with cal applied
 *   - panel dim red, one
 *     ring blinking bright   : failure — the blinking ring is the jack
 *                              that failed (no blinking ring = failure
 *                              before the sweeps). Power-cycle, retry.
 *                              A previous good record survives measurement failure
 *                              (a torn QSPI write fails CRC on the next
 *                              boot and falls back cleanly).
 *
 * Preconditions: nothing patched into any CV jack (the zero pass measures
 * open-jack idle). No warm-up wait is required by the procedure.
 *
 * For automated paired CV and codec testing with USB reports, use the
 * separate alchemy-calibrator production station. Its paired harness must
 * not be used with this unpatched button procedure.
 */

#pragma once

#include "alchemy/hw/v2_calibration.h"

namespace alchemy {

class AlchemyLabV2;

/** Exclusive raw ADC access for calibration. Stop the streaming ADC before
 * Init; these readings bypass any previously loaded calibration. Do not restart
 * DMA until the calibration ADC has been relinquished (normally by reboot).
 * Each read starts and waits for a new conversion; no delayed DMA snapshots. */
bool V2CalibrationAdcInit();
bool V2CalibrationReadJack(uint8_t jack, uint16_t& raw);
bool V2CalibrationReadReference(uint16_t& raw);

/** Validates, persists, restores QSPI mapping and verifies the record.
 * Caller must run from SRAM, stop audio/other QSPI clients, and park outputs.
 * Measurement failures never enter here; a power loss during this single-sector
 * write can invalidate the old record. The loader rejects torn records. */
bool V2PersistCalibration(AlchemyLabV2& hw, const V2Calibration& cal);

/** True when B1 and B2 are both held (raw GPIO read with pull-ups —
 *  callable before any board init beyond DaisySeed::Init()). */
bool V2FactoryCalRequested();

/** Run the full cal procedure, persist to QSPI, soft-reset. Never
 *  returns. On unrecoverable failure, traps in a rapid-blink loop. */
[[noreturn]] void V2RunFactoryCalibrationAndReset(AlchemyLabV2& hw);

} // namespace alchemy
