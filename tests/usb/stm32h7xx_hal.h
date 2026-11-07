#pragma once
#include <cstdint>
struct PCD_HandleTypeDef { void* Instance = nullptr; };
struct HCD_HandleTypeDef { void* Instance = nullptr; };
enum HAL_StatusTypeDef { HAL_OK,HAL_ERROR };
inline void HAL_PWREx_EnableUSBVoltageDetector() {}
inline HAL_StatusTypeDef HAL_PCDEx_SetRxFiFo(PCD_HandleTypeDef*,uint16_t) { return HAL_OK; }
inline HAL_StatusTypeDef HAL_PCDEx_SetTxFiFo(PCD_HandleTypeDef*,uint8_t,uint16_t) { return HAL_OK; }
inline uint32_t HAL_GetUIDw0() { return 1; }
inline uint32_t HAL_GetUIDw1() { return 2; }
inline uint32_t HAL_GetUIDw2() { return 3; }
inline void HAL_NVIC_SystemReset() {}
