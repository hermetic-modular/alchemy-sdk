#include "alchemy/usb/device_class.h"
extern "C" {
#include "usbd_ctlreq.h"
#include "usbd_ioreq.h"
}
#include <algorithm>

namespace alchemy::usb {
USBD_ClassTypeDef& DeviceClass::Driver() {
    static USBD_ClassTypeDef driver = [] {
        USBD_ClassTypeDef value{};
        value.Init = Init; value.DeInit = DeInit; value.Setup = Setup;
        value.EP0_RxReady = Ep0Ready; value.DataIn = DataIn; value.DataOut = DataOut;
        value.GetFSConfigDescriptor = Configuration; value.GetHSConfigDescriptor = Configuration;
        value.GetOtherSpeedConfigDescriptor = Configuration; value.GetDeviceQualifierDescriptor = Qualifier;
        return value;
    }();
    return driver;
}
void DeviceClass::Attach(USBD_HandleTypeDef& device) {
    device_ = &device; device.pUserData[0] = this;
}
void DeviceClass::AdvanceBusGeneration() {
    generation_.fetch_add(1, std::memory_order_release);
    cdc_generation_.fetch_add(1, std::memory_order_release);
    midi_generation_.fetch_add(1, std::memory_order_release);
}
void DeviceClass::Refresh() {
    if (!Configured() || !device_) return;
    if (device_->dev_state == USBD_STATE_SUSPENDED) {
        if (!suspended_) {
            available_.store(false,std::memory_order_release); suspended_ = true;
            AdvanceBusGeneration();
            cdc_tx_.RetirePending(); midi_tx_.RetirePending();
            notify_dirty_ = true;
        }
    } else if (suspended_ && device_->dev_state == USBD_STATE_CONFIGURED) {
        suspended_ = false; available_.store(true,std::memory_order_release);
    }
}
DeviceClass& DeviceClass::Self(USBD_HandleTypeDef* device) {
    return *static_cast<DeviceClass*>(device->pUserData[0]);
}
uint8_t* DeviceClass::Configuration(uint16_t* size) {
    static std::array<uint8_t,sizeof(kConfiguration)> descriptor = [] {
        std::array<uint8_t,sizeof(kConfiguration)> value{};
        std::copy(std::begin(kConfiguration),std::end(kConfiguration),value.begin()); return value;
    }();
    *size = descriptor.size(); return descriptor.data();
}
uint8_t* DeviceClass::Qualifier(uint16_t* size) {
    static uint8_t qualifier[] = {10,6,0,2,0xef,2,1,64,1,0};
    *size = sizeof(qualifier); return qualifier;
}
uint8_t DeviceClass::Init(USBD_HandleTypeDef* device, uint8_t) {
    auto& self = Self(device);
    self.cdc_tx_.Reset(); self.midi_tx_.Reset();
    self.line_pending_ = self.cdc_zlp_ = self.cdc_zlp_pending_ = false;
    self.notify_busy_ = self.notify_dirty_ = false;
    self.control_lines_ = 0; self.suspended_ = false;
    for (const auto endpoint : {kCdcOut,kCdcIn,kCdcNotify,kMidiOut,kMidiIn}) {
        const uint16_t size = endpoint == kCdcNotify ? 8 : kPacketSize;
        const auto type = endpoint == kCdcNotify ? USBD_EP_TYPE_INTR : USBD_EP_TYPE_BULK;
        if (USBD_LL_OpenEP(device, endpoint, uint8_t(type), size) != USBD_OK) {
            DeInit(device,0); return USBD_FAIL;
        }
        auto& ep = endpoint & 0x80 ? device->ep_in[endpoint & 15] : device->ep_out[endpoint];
        ep.is_used = 1; ep.maxpacket = size;
    }
    if (USBD_LL_PrepareReceive(device,kCdcOut,self.cdc_rx_.data(),kPacketSize) != USBD_OK ||
        USBD_LL_PrepareReceive(device,kMidiOut,self.midi_rx_.data(),kPacketSize) != USBD_OK) {
        DeInit(device,0); return USBD_FAIL;
    }
    self.AdvanceBusGeneration();
    self.available_.store(true,std::memory_order_release);
    self.configured_.store(true,std::memory_order_release);
    return USBD_OK;
}
uint8_t DeviceClass::DeInit(USBD_HandleTypeDef* device, uint8_t) {
    auto& self = Self(device);
    self.configured_.store(false,std::memory_order_release); self.available_.store(false,std::memory_order_release);
    self.suspended_ = false;
    self.AdvanceBusGeneration();
    for (const auto endpoint : {kCdcOut,kCdcIn,kCdcNotify,kMidiOut,kMidiIn}) {
        auto& ep = endpoint & 0x80 ? device->ep_in[endpoint & 15] : device->ep_out[endpoint];
        if (ep.is_used) USBD_LL_CloseEP(device,endpoint);
        ep.is_used = 0;
    }
    self.cdc_tx_.Reset(); self.midi_tx_.Reset();
    return USBD_OK;
}
uint8_t DeviceClass::Setup(USBD_HandleTypeDef* device, USBD_SetupReqTypedef* req) {
    auto& self = Self(device);
    const auto fail = [&] { USBD_CtlError(device,req); return uint8_t(USBD_FAIL); };
    if ((req->bmRequest & USB_REQ_RECIPIENT_MASK) == USB_REQ_RECIPIENT_ENDPOINT) {
        if (req->bRequest == USB_REQ_CLEAR_FEATURE && req->wValue == USB_FEATURE_EP_HALT) {
            if (req->wIndex == kCdcIn || req->wIndex == kMidiIn || req->wIndex == kCdcNotify) {
                if (req->wIndex == kCdcIn) self.cdc_tx_.RetirePending();
                else if (req->wIndex == kMidiIn) {
                    self.midi_tx_.RetirePending(); self.midi_generation_.fetch_add(1,std::memory_order_release);
                } else self.notify_dirty_ = true;
            }
            return USBD_OK;
        }
        return fail();
    }
    if (req->wIndex > 3) return fail();
    if ((req->bmRequest & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_CLASS) {
        if (req->wIndex != 0) return fail();
        switch (req->bRequest) {
            case 0x20:
                if (req->bmRequest != 0x21 || req->wLength != 7 || req->wValue) return fail();
                self.line_pending_ = true;
                return uint8_t(USBD_CtlPrepareRx(device,self.pending_line_.data(),7));
            case 0x21:
                if (req->bmRequest != 0xa1 || req->wLength != 7 || req->wValue) return fail();
                return uint8_t(USBD_CtlSendData(device,self.line_coding_.data(),7));
            case 0x22:
                if (req->bmRequest != 0x21 || req->wLength || req->wValue > 3) return fail();
                if ((self.control_lines_ ^ req->wValue) & 1) {
                    self.cdc_generation_.fetch_add(1,std::memory_order_release);
                }
                self.control_lines_ = req->wValue; self.notify_dirty_ = true; return USBD_OK;
            default: return fail();
        }
    }
    if ((req->bmRequest & USB_REQ_TYPE_MASK) == USB_REQ_TYPE_STANDARD) {
        static uint8_t alternate = 0; static uint8_t status[2] = {0,0};
        if (req->bRequest == USB_REQ_GET_INTERFACE && req->bmRequest == 0x81 && req->wLength == 1 && !req->wValue)
            return uint8_t(USBD_CtlSendData(device,&alternate,1));
        if (req->bRequest == USB_REQ_SET_INTERFACE && req->bmRequest == 1 && !req->wLength && !req->wValue)
            return USBD_OK;
        if (req->bRequest == USB_REQ_GET_STATUS && req->bmRequest == 0x81 && req->wLength == 2 && !req->wValue)
            return uint8_t(USBD_CtlSendData(device,status,2));
    }
    return fail();
}
uint8_t DeviceClass::Ep0Ready(USBD_HandleTypeDef* device) {
    auto& self = Self(device);
    if (!self.line_pending_) return USBD_FAIL;
    self.line_pending_ = false;
    if (USBD_LL_GetRxDataSize(device,0) != self.line_coding_.size()) {
        USBD_CtlError(device,&device->request); return USBD_FAIL;
    }
    std::copy_n(self.pending_line_.begin(),self.line_coding_.size(),self.line_coding_.begin());
    return USBD_OK;
}
uint8_t DeviceClass::DataIn(USBD_HandleTypeDef* device, uint8_t endpoint) {
    auto& self = Self(device);
    self.Refresh(); if (!self.Available()) return USBD_OK;
    endpoint &= 15;
    if (endpoint == (kCdcIn & 15)) {
        if (self.cdc_zlp_) {
            self.cdc_zlp_ = false; self.cdc_zlp_pending_ = true;
        } else self.cdc_tx_.Complete();
    } else if (endpoint == (kMidiIn & 15)) self.midi_tx_.Complete();
    else if (endpoint == (kCdcNotify & 15)) self.notify_busy_ = false;
    else return USBD_FAIL;
    self.Pump();
    return USBD_OK;
}
uint8_t DeviceClass::DataOut(USBD_HandleTypeDef* device, uint8_t endpoint) {
    auto& self = Self(device);
    self.Refresh(); if (!self.Available()) return USBD_FAIL;
    auto* buffer = endpoint == kCdcOut ? self.cdc_rx_.data() : self.midi_rx_.data();
    if (endpoint != kCdcOut && endpoint != kMidiOut) return USBD_FAIL;
    const auto count = USBD_LL_GetRxDataSize(device,endpoint);
    const auto& receiver = endpoint == kCdcOut ? self.cdc_receiver_ : self.midi_receiver_;
    if (count > kPacketSize) return USBD_FAIL;
    if (receiver.receive && count) receiver.receive(receiver.context,buffer,count);
    return uint8_t(USBD_LL_PrepareReceive(device,endpoint,buffer,kPacketSize));
}
bool DeviceClass::SendCdc(const uint8_t* bytes, size_t size) {
    Refresh(); return Available() && cdc_tx_.Submit(bytes,size);
}
bool DeviceClass::SendMidi(const uint8_t* packets, size_t size, bool realtime) {
    Refresh(); return Available() && size % 4 == 0 && midi_tx_.Submit(packets,size,realtime);
}
void DeviceClass::Pump() {
    Refresh(); if (!Available()) return;
    if (cdc_zlp_pending_) {
        if (USBD_LL_Transmit(device_,kCdcIn,nullptr,0) == USBD_OK) cdc_zlp_pending_ = false;
    } else cdc_tx_.Pump([&](uint8_t* bytes,uint16_t size) {
        cdc_zlp_ = size % kPacketSize == 0;
        const auto result = USBD_LL_Transmit(device_,kCdcIn,bytes,size);
        if (result != USBD_OK) cdc_zlp_ = false;
        return result == USBD_OK;
    });
    midi_tx_.Pump([&](uint8_t* bytes,uint16_t size) { return USBD_LL_Transmit(device_,kMidiIn,bytes,size) == USBD_OK; },true);
    if (notify_dirty_ && !notify_busy_) {
        notification_[8] = control_lines_ & 1 ? 3 : 0;
        notify_busy_ = true;
        if (USBD_LL_Transmit(device_,kCdcNotify,notification_.data(),notification_.size()) != USBD_OK) notify_busy_ = false;
        else notify_dirty_ = false;
    }
}
}
