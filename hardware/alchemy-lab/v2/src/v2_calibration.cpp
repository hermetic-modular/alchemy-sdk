/**
 * @file v2_calibration.cpp
 * @brief V2 calibration record: portable CRC/fallback and QSPI load.
 *
 * Keep these definitions in the original source file for firmware Makefiles
 * that enumerate SDK sources explicitly. Host tests compile the same portable
 * code; only the memory-mapped hardware reader is excluded there.
 */

#include "alchemy/hw/v2_calibration.h"
#include <cstring>

namespace alchemy {
uint32_t V2CalCrc32(const void* data, size_t len)
{
    constexpr uint32_t kPolynomial = 0xEDB88320u;
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i)
    {
        crc ^= p[i];
        for (int j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ ((crc & 1u) ? kPolynomial : 0u);
    }
    return crc ^ 0xFFFFFFFFu;
}

void V2CalDesignFallback(V2Calibration& out)
{
    std::memset(&out, 0, sizeof(out));
    out.magic          = kV2CalMagic;
    out.schema_version = kV2CalSchemaVersion;
    out.vdda_at_cal    = kV2VddaDesign;
    out.vdda_source    = kV2VddaAssumed;

    /* Design transfer: V_jack = 5 − 3.03 × (code/4096 × 3.30)
     *   → slope = −3.03 × 3.30 / 4096, offset = +5.0 */
    const float design_slope =
        kV2CvOutGainDesign * kV2VddaDesign / 4096.0f;

    for (uint8_t i = 0; i < kV2CalNumJacks; ++i)
    {
        out.jack[i].dac_offset_v        = kV2CvOutOffsetDesignV;
        out.jack[i].dac_gain_v_per_code = design_slope;
        out.jack[i].dac_min_linear_code = 0u;
        out.jack[i].dac_max_linear_code = 4095u;
        out.jack[i].adc_zero_code       = 32768u;  /* mid-scale */
    }
    out.crc32 = V2CalComputeCrc(out);
}


} // namespace alchemy

#if !defined(ALCHEMY_CALIBRATION_HOST_TEST)
#include "alchemy/hw/v2_calibration_checks.h"
#include "daisy_seed.h"

namespace alchemy {

bool V2CalLoadFromQspi(V2Calibration& out)
{
    /* QSPI is memory-mapped after DaisySeed::Init() (BOOT_SRAM apps).
     * The D-cache may hold stale lines for the region — e.g. right after
     * a factory-cal write — so invalidate before reading. The address is
     * sector-aligned; round the length up to a full cache line. */
    SCB_InvalidateDCache_by_Addr(
        reinterpret_cast<uint32_t*>(kV2CalQspiAddr),
        static_cast<int32_t>((sizeof(V2Calibration) + 31u) & ~31u));

    std::memcpy(&out,
                reinterpret_cast<const void*>(kV2CalQspiAddr),
                sizeof(V2Calibration));

    if (out.magic != kV2CalMagic)                   return false;
    if (out.schema_version != kV2CalSchemaVersion)  return false;
    if (V2CalComputeCrc(out) != out.crc32)          return false;
    return V2CalRecordPlausible(out);
}

} // namespace alchemy

#endif
