/**
 * @file alchemy_lab_v1_layout.cpp
 * @brief Alchemy Lab V1 — HardwareLayout instance and platform helpers.
 *
 * Defines the static ring/button chain layout tables and populates
 * kAlchemyLabV1Layout.  All layout data is constexpr; the extern
 * definition just aggregates pointers to those tables.
 *
 * Also provides PresetFlashOps() which wraps daisy::QSPIHandle for use
 * with alchemy::PresetStore.
 */

#include "alchemy/hw/alchemy_lab_v1_layout.h"

/* D-cache invalidation helper provided by libDaisy / CMSIS. */
extern "C" void dsy_dma_invalidate_cache_for_buffer(uint8_t* buf_ptr, size_t size);

namespace alchemy {

namespace {

constexpr float kStepCW  =  0.75f;
constexpr float kStepCCW = -0.75f;

/* ── Per-pot ring chain layout ─────────────────────────────────────────── */
/*
 * Fields: { chain_start, led_count, start_hour, step_hours }
 *
 * chain_start: index of LED at ring offset 0 in the WS2812 chain.
 * start_hour:  clock-face hour of that first LED.
 * step_hours:  clock-face hours per subsequent LED (CW = +0.75, CCW = -0.75).
 */
constexpr LedRingLayout kRings[6] = {
    /* Pot 1 */ { 69, 16,  3.00f, kStepCW  },
    /* Pot 2 */ { 86, 16,  9.75f, kStepCW  },
    /* Pot 3 */ { 52, 16,  2.25f, kStepCW  },
    /* Pot 4 */ { 35, 16,  9.00f, kStepCCW },
    /* Pot 5 */ { 17, 16,  3.00f, kStepCW  },
    /* Pot 6 */ {  0, 16,  9.00f, kStepCW  },
};

/* ── Per-button accent LED chain indices ───────────────────────────────── */
/*
 * Fields: { bottom_chain, top_chain }
 *
 * Both LEDs are part of the same WS2812 chain; the index is their position
 * within it.  They sit physically above and below the button cap.
 */
constexpr LedButtonLayout kButtons[3] = {
    /* B1 */ { 68, 85 },
    /* B2 */ { 34, 51 },
    /* B3 */ { 16, 33 },
};

} // namespace

/* ── PresetFlashOps ─────────────────────────────────────────────────────── */

namespace {

struct QspiCtx { daisy::QSPIHandle* qspi; };

/*
 * s_qspi_ctx is file-scope so that PresetFlashOps() can return a FlashOps
 * struct whose void* ctx pointer outlives the call.  The three C-style
 * function pointers (v1_erase, v1_write, v1_invalidate) have no userdata
 * parameter of their own — the ctx pointer IS the userdata — so the context
 * object must have static storage duration.  Single-instance per board.
 */
QspiCtx s_qspi_ctx;

bool v1_erase(void* ctx, uintptr_t start, uintptr_t end)
{
    auto* c = static_cast<QspiCtx*>(ctx);
    return c->qspi->Erase(start, end) == daisy::QSPIHandle::Result::OK;
}

bool v1_write(void* ctx, uintptr_t addr, const uint8_t* buf, uint32_t len)
{
    auto* c = static_cast<QspiCtx*>(ctx);
    /* QSPIHandle::Write takes a non-const pointer; the cast is safe because
     * the underlying DMA only reads from the buffer. */
    return c->qspi->Write(addr, len, const_cast<uint8_t*>(buf))
           == daisy::QSPIHandle::Result::OK;
}

void v1_invalidate(void* ctx, uintptr_t addr, uint32_t len)
{
    (void)ctx;
    dsy_dma_invalidate_cache_for_buffer(
        reinterpret_cast<uint8_t*>(addr),
        static_cast<size_t>(len));
}

} // namespace

FlashOps PresetFlashOps(daisy::QSPIHandle* qspi)
{
    s_qspi_ctx.qspi = qspi;
    return { v1_erase, v1_write, v1_invalidate, &s_qspi_ctx };
}

/* ── HardwareLayout instance ───────────────────────────────────────────── */

const HardwareLayout kAlchemyLabV1Layout = {
    .num_pots  = kNumPots,
    .num_cv    = kNumCvInputs,
    .num_btns  = kNumButtons,

    .led_total = kLedTotal,
    .rings     = kRings,
    .buttons   = kButtons,
};

} // namespace alchemy
