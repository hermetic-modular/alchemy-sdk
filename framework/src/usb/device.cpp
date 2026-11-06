#include "alchemy/usb/device.h"
#include "util/scopedirqblocker.h"
#include "stm32h7xx_hal.h"
extern "C" {
#include "usbd_desc.h"
#include "usbd_ctlreq.h"
extern USBD_HandleTypeDef hUsbDeviceFS, hUsbDeviceHS;
extern HCD_HandleTypeDef hhcd_USB_OTG_HS;
}
#include <cstring>

namespace alchemy::usb {
namespace {
char product_name[49] = "Alchemy";
alignas(4) uint8_t string_buffer[100];
alignas(4) uint8_t device_descriptor[] = {18,1,0,2,0xef,2,1,64,0x83,4,0x40,0x57,0,1,1,2,3,1};
void SetDescriptorWord(size_t offset, uint16_t value) {
    device_descriptor[offset] = uint8_t(value);
    device_descriptor[offset + 1] = uint8_t(value >> 8);
}
uint8_t* DeviceDescriptor(USBD_SpeedTypeDef, uint16_t* size) { *size = sizeof(device_descriptor); return device_descriptor; }
uint8_t* String(const char* value, uint16_t* size) {
    USBD_GetString(reinterpret_cast<uint8_t*>(const_cast<char*>(value)),string_buffer,size); return string_buffer;
}
uint8_t* Product(USBD_SpeedTypeDef, uint16_t* size) { return String(product_name,size); }
uint8_t* CdcName(USBD_SpeedTypeDef, uint16_t* size) { return String("HostLink",size); }
uint8_t* MidiName(USBD_SpeedTypeDef, uint16_t* size) { return String("MIDI",size); }
USBD_DescriptorsTypeDef descriptors{};
bool ValidProduct(const char* value) {
    if (!value) return true;
    const auto length = std::strlen(value);
    if (!length || length > 48) return false;
    for (size_t i = 0; i < length; ++i) if (uint8_t(value[i]) < 32 || uint8_t(value[i]) > 126) return false;
    return true;
}
}
Device& Device::Shared() { static Device value; return value; }
bool Device::Configure(daisy::UsbHandle::UsbPeriph peripheral, const char* product, Identity identity) {
    if (initialized_ || (peripheral != daisy::UsbHandle::FS_INTERNAL && peripheral != daisy::UsbHandle::FS_EXTERNAL) || !ValidProduct(product)) return false;
    peripheral_ = peripheral; identity_configured_ = true;
    SetDescriptorWord(8, identity.vendor_id);
    SetDescriptorWord(10, identity.product_id);
    SetDescriptorWord(12, identity.device_version);
    const char* name = product ? product : "Alchemy";
    std::memcpy(product_name,name,std::strlen(name)+1);
    return true;
}
bool Device::Init(daisy::UsbHandle::UsbPeriph peripheral, const char* product, const char* fallback_product) {
    if (!product && !initialized_ && !identity_configured_) product = fallback_product;
    if ((peripheral != daisy::UsbHandle::FS_INTERNAL && peripheral != daisy::UsbHandle::FS_EXTERNAL) || !ValidProduct(product)) return false;
    if (identity_configured_ || initialized_) {
        if (peripheral != peripheral_ || (product && std::strcmp(product_name, product))) return false;
        if (initialized_) return true;
    }
    if (peripheral == daisy::UsbHandle::FS_EXTERNAL && hhcd_USB_OTG_HS.Instance) return false;
    auto* handle = peripheral == daisy::UsbHandle::FS_INTERNAL ? &hUsbDeviceFS : &hUsbDeviceHS;
    if (handle->pClass[0] && (handle != handle_ || handle->pClass[0] != &DeviceClass::Driver()
        || (handle->pUserData[0] && handle->pUserData[0] != &class_))) return false;
    peripheral_ = peripheral; handle_ = handle;
    descriptors = peripheral == daisy::UsbHandle::FS_INTERNAL ? FS_Desc : HS_Desc;
    descriptors.GetDeviceDescriptor = DeviceDescriptor;
    descriptors.GetProductStrDescriptor = Product;
    descriptors.GetConfigurationStrDescriptor = CdcName;
    descriptors.GetInterfaceStrDescriptor = MidiName;
    if (product) std::memcpy(product_name,product,std::strlen(product)+1);
    HAL_PWREx_EnableUSBVoltageDetector();
    if (USBD_Init(handle_,&descriptors,peripheral == daisy::UsbHandle::FS_INTERNAL ? DEVICE_FS : DEVICE_HS) != USBD_OK) return false;
    auto* pcd = static_cast<PCD_HandleTypeDef*>(handle_->pData);
    if (HAL_PCDEx_SetRxFiFo(pcd,128) != HAL_OK || HAL_PCDEx_SetTxFiFo(pcd,0,32) != HAL_OK ||
        HAL_PCDEx_SetTxFiFo(pcd,1,64) != HAL_OK || HAL_PCDEx_SetTxFiFo(pcd,2,16) != HAL_OK ||
        HAL_PCDEx_SetTxFiFo(pcd,3,64) != HAL_OK) { USBD_DeInit(handle_); return false; }
    class_.Attach(*handle_);
    if (USBD_RegisterClass(handle_,&DeviceClass::Driver()) != USBD_OK || USBD_Start(handle_) != USBD_OK) {
        USBD_DeInit(handle_); return false;
    }
    initialized_ = true;
    return true;
}
void Device::Stop() {
    if (!initialized_) return;
    daisy::ScopedIrqBlocker guard;
    USBD_Stop(handle_); USBD_DeInit(handle_);
    static_cast<PCD_HandleTypeDef*>(handle_->pData)->Instance = nullptr;
    initialized_ = false;
}
bool Device::SetCdcReceiver(ReceiveCallback receiver) {
    daisy::ScopedIrqBlocker guard;
    return class_.SetCdcReceiver(receiver);
}
bool Device::SetMidiReceiver(ReceiveCallback receiver) {
    daisy::ScopedIrqBlocker guard;
    return class_.SetMidiReceiver(receiver);
}
bool Device::SendCdc(const uint8_t* bytes, size_t size) {
    daisy::ScopedIrqBlocker guard;
    return class_.SendCdc(bytes,size);
}
bool Device::SendMidi(const uint8_t* packets, size_t size, bool realtime) {
    return SendMidi(packets,size,realtime,MidiGeneration());
}
bool Device::SendMidi(const uint8_t* packets, size_t size, bool realtime, uint32_t generation) {
    daisy::ScopedIrqBlocker guard;
    class_.Refresh();
    return class_.MidiGeneration() == generation && class_.SendMidi(packets,size,realtime);
}
void Device::Refresh() {
    if (!initialized_) return;
    daisy::ScopedIrqBlocker guard; class_.Refresh();
}
void Device::Poll() {
    if (!initialized_) return;
    daisy::ScopedIrqBlocker guard; class_.Pump();
}
}
