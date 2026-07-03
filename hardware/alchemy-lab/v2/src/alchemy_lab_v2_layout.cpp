/**
 * @file alchemy_lab_v2_layout.cpp
 * @brief Alchemy Lab V2 — HardwareLayout instance and QSPI flash helper.
 */

#include "alchemy/hw/alchemy_lab_v2_layout.h"

extern "C" void dsy_dma_invalidate_cache_for_buffer(uint8_t* buf_ptr, size_t size);

namespace alchemy {

namespace {

constexpr float kStepCW  =  0.75f;
constexpr float kStepCCW = -0.75f;

constexpr LedRingLayout kRings[6] = {
    /* Pot 1 */ { 69, 16,  3.00f, kStepCW  },
    /* Pot 2 */ { 86, 16,  9.75f, kStepCW  },
    /* Pot 3 */ { 52, 16,  2.25f, kStepCW  },
    /* Pot 4 */ { 35, 16,  9.00f, kStepCCW },
    /* Pot 5 */ { 17, 16,  3.00f, kStepCW  },
    /* Pot 6 */ {  0, 16,  9.00f, kStepCW  },
};

constexpr LedButtonLayout kButtons[3] = {
    /* B1 */ { 68, 85 },
    /* B2 */ { 34, 51 },
    /* B3 */ { 16, 33 },
};

} // namespace

/* ── PresetFlashOps ───── */

namespace {

struct QspiCtx { daisy::QSPIHandle* qspi; };

QspiCtx s_qspi_ctx;

bool v2_erase(void* ctx, uintptr_t start, uintptr_t end)
{
    auto* c = static_cast<QspiCtx*>(ctx);
    return c->qspi->Erase(start, end) == daisy::QSPIHandle::Result::OK;
}

bool v2_write(void* ctx, uintptr_t addr, const uint8_t* buf, uint32_t len)
{
    auto* c = static_cast<QspiCtx*>(ctx);
    return c->qspi->Write(addr, len, const_cast<uint8_t*>(buf))
           == daisy::QSPIHandle::Result::OK;
}

void v2_invalidate(void* /*ctx*/, uintptr_t addr, uint32_t len)
{
    dsy_dma_invalidate_cache_for_buffer(
        reinterpret_cast<uint8_t*>(addr),
        static_cast<size_t>(len));
}

} // namespace

FlashOps PresetFlashOps(daisy::QSPIHandle* qspi)
{
    s_qspi_ctx.qspi = qspi;
    return { v2_erase, v2_write, v2_invalidate, &s_qspi_ctx };
}

const HardwareLayout kAlchemyLabV2Layout = {
    .num_pots  = kNumPots,
    .num_cv    = kNumCvInputs,
    .num_btns  = kNumButtons,

    .led_total = kLedTotal,
    .rings     = kRings,
    .buttons   = kButtons,
};

} // namespace alchemy
