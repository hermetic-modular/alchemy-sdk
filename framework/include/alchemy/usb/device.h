#pragma once
#include "alchemy/usb/device_class.h"
#include "hid/usb.h"

namespace alchemy::usb {
class Device {
  public:
    static Device& Shared();
    struct Identity {
        // device_version is the USB descriptor bcdDevice value.
        uint16_t vendor_id = 0x0483, product_id = 0x5740, device_version = 0x0100;
    };
    // Call in the foreground before either service starts. The product name is copied.
    bool Configure(daisy::UsbHandle::UsbPeriph peripheral, const char* product) {
        return Configure(peripheral,product,Identity{});
    }
    bool Configure(daisy::UsbHandle::UsbPeriph peripheral, const char* product, Identity identity);
    // A fallback names a new identity only; an existing/configured identity wins.
    bool Init(daisy::UsbHandle::UsbPeriph peripheral, const char* product = nullptr,
              const char* fallback_product = nullptr);
    void Stop();
    // Replace or release only the same context; release with {owner, nullptr}.
    bool SetCdcReceiver(ReceiveCallback receiver);
    bool SetMidiReceiver(ReceiveCallback receiver);
    bool SendCdc(const uint8_t* bytes, size_t size);
    bool SendMidi(const uint8_t* packets, size_t size, bool realtime = false);
    bool SendMidi(const uint8_t* packets, size_t size, bool realtime, uint32_t generation);
    void Refresh();
    void Poll();
    bool Available() const { return class_.Available(); }
    bool Suspended() const { return class_.Suspended(); }
    bool Configured() const { return class_.Configured(); }
    bool Initialized() const { return initialized_; }
    uint32_t Generation() const { return class_.Generation(); }
    uint32_t CdcGeneration() const { return class_.CdcGeneration(); }
    uint32_t MidiGeneration() const { return class_.MidiGeneration(); }
    size_t CdcSpace() const { return Available() ? class_.CdcSpace() : 0; }
    uint32_t MidiCompleted() const { return class_.MidiCompleted(); }
    uint32_t MidiCancelled() const { return class_.MidiCancelled(); }
  private:
    Device() = default;
    DeviceClass class_;
    USBD_HandleTypeDef* handle_ = nullptr;
    daisy::UsbHandle::UsbPeriph peripheral_ = daisy::UsbHandle::FS_INTERNAL;
    bool initialized_ = false, identity_configured_ = false;
};
}
