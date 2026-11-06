#include "alchemy/usb/device_class.h"
#include "stm32h7xx_hal.h"
#include "fixture.h"
#include <cassert>
extern "C" {
USBD_HandleTypeDef hUsbDeviceFS{},hUsbDeviceHS{};
USBD_DescriptorsTypeDef FS_Desc{},HS_Desc{};
HCD_HandleTypeDef hhcd_USB_OTG_HS{};
}
static PCD_HandleTypeDef pcd{};
Endpoint in[16],out[16];
bool busy = false, fail_start = false;
uint8_t busy_endpoint = 0;
extern "C" {
USBD_StatusTypeDef USBD_LL_Init(USBD_HandleTypeDef* device) { device->pData = &pcd; return USBD_OK; }
USBD_StatusTypeDef USBD_LL_DeInit(USBD_HandleTypeDef*) { return USBD_OK; }
USBD_StatusTypeDef USBD_LL_Start(USBD_HandleTypeDef*) { return fail_start ? USBD_FAIL : USBD_OK; }
USBD_StatusTypeDef USBD_LL_Stop(USBD_HandleTypeDef*) { return USBD_OK; }
USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef*,uint8_t ep,uint8_t,uint16_t) { (ep&128?in[ep&15]:out[ep]).open=true; return USBD_OK; }
USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef*,uint8_t ep) { auto& endpoint=ep&128?in[ep&15]:out[ep]; endpoint.open=false; endpoint.active=false; return USBD_OK; }
USBD_StatusTypeDef USBD_LL_FlushEP(USBD_HandleTypeDef*,uint8_t ep) { auto& endpoint=ep&128?in[ep&15]:out[ep]; ++endpoint.flushed; endpoint.active=false; return USBD_OK; }
USBD_StatusTypeDef USBD_LL_StallEP(USBD_HandleTypeDef*,uint8_t ep) { (ep&128?in[ep&15]:out[ep]).stalled=true; return USBD_OK; }
USBD_StatusTypeDef USBD_LL_ClearStallEP(USBD_HandleTypeDef*,uint8_t ep) { (ep&128?in[ep&15]:out[ep]).stalled=false; return USBD_OK; }
USBD_StatusTypeDef USBD_LL_SetUSBAddress(USBD_HandleTypeDef*,uint8_t) { return USBD_OK; }
uint8_t USBD_LL_IsStallEP(USBD_HandleTypeDef*,uint8_t ep) { return (ep&128?in[ep&15]:out[ep]).stalled; }
uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef*,uint8_t ep) { return out[ep].received; }
USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef*,uint8_t ep,uint8_t* bytes,uint32_t size) {
    if (ep && (busy || ep == busy_endpoint)) return USBD_BUSY;
    assert(!ep || !in[ep&15].active);
    in[ep&15].buffer=bytes; in[ep&15].size=size; in[ep&15].active=true; return USBD_OK;
}
USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef*,uint8_t ep,uint8_t* bytes,uint32_t size) {
    out[ep].buffer=bytes; out[ep].size=size; return USBD_OK;
}
void USBD_LL_Delay(uint32_t) {}
}

void CompleteIn(USBD_HandleTypeDef& device, uint8_t endpoint) {
    endpoint &= 15;
    auto& transfer = in[endpoint];
    assert(transfer.active);
    auto* buffer = transfer.buffer;
    transfer.active = false;
    assert(USBD_LL_DataInStage(&device,endpoint,buffer) == USBD_OK);
}
