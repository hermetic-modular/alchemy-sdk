#pragma once
#include "alchemy/usb/descriptors.h"
#include "alchemy/usb/endpoint_tx.h"
extern "C" {
#include "usbd_core.h"
}

namespace alchemy::usb {
struct ReceiveCallback {
    void* context = nullptr;
    void (*receive)(void*, const uint8_t*, size_t) = nullptr;
};

class DeviceClass {
  public:
    static USBD_ClassTypeDef& Driver();
    void Attach(USBD_HandleTypeDef& device);
    bool SetCdcReceiver(ReceiveCallback receiver) { return SetReceiver(cdc_receiver_,receiver); }
    bool SetMidiReceiver(ReceiveCallback receiver) { return SetReceiver(midi_receiver_,receiver); }
    bool SendCdc(const uint8_t* bytes, size_t size);
    bool SendMidi(const uint8_t* packets, size_t size, bool realtime = false);
    size_t CdcSpace() const { return cdc_tx_.Space(); }
    // Serialized with USB IRQs by Device. Observe ST suspend/resume before sending.
    void Refresh();
    void Pump();
    bool Available() const { return Configured() && available_.load(std::memory_order_acquire) && device_ && device_->dev_state == USBD_STATE_CONFIGURED; }
    bool Suspended() const { return Configured() && device_ && device_->dev_state == USBD_STATE_SUSPENDED; }
    bool Configured() const { return configured_.load(std::memory_order_acquire); }
    uint32_t Generation() const { return generation_.load(std::memory_order_acquire); }
    uint32_t CdcGeneration() const { return cdc_generation_.load(std::memory_order_acquire); }
    uint32_t MidiGeneration() const { return midi_generation_.load(std::memory_order_acquire); }
    uint32_t MidiCompleted() const { return midi_tx_.Completed(); }
    uint32_t MidiCancelled() const { return midi_tx_.Cancelled(); }
  private:
    static bool SetReceiver(ReceiveCallback& current, ReceiveCallback next) {
        if (current.receive && current.context != next.context) return false;
        current = next.receive ? next : ReceiveCallback{};
        return true;
    }
    static DeviceClass& Self(USBD_HandleTypeDef* device);
    void AdvanceBusGeneration();
    static uint8_t Init(USBD_HandleTypeDef*, uint8_t);
    static uint8_t DeInit(USBD_HandleTypeDef*, uint8_t);
    static uint8_t Setup(USBD_HandleTypeDef*, USBD_SetupReqTypedef*);
    static uint8_t Ep0Ready(USBD_HandleTypeDef*);
    static uint8_t DataIn(USBD_HandleTypeDef*, uint8_t);
    static uint8_t DataOut(USBD_HandleTypeDef*, uint8_t);
    static uint8_t* Configuration(uint16_t*);
    static uint8_t* Qualifier(uint16_t*);
    USBD_HandleTypeDef* device_ = nullptr;
    ReceiveCallback cdc_receiver_{}, midi_receiver_{};
    EndpointTx<32,256> cdc_tx_;
    EndpointTx<32,kPacketSize> midi_tx_;
    alignas(4) std::array<uint8_t,kPacketSize> cdc_rx_{}, midi_rx_{};
    alignas(4) std::array<uint8_t,7> line_coding_{{0,0xc2,1,0,0,0,8}};
    // ST arms a full EP0 OUT packet even for the seven-byte control payload.
    alignas(4) std::array<uint8_t,64> pending_line_{};
    alignas(4) std::array<uint8_t,10> notification_{{0xa1,0x20,0,0,0,0,2,0,0,0}};
    std::atomic<bool> configured_{false}, available_{false};
    std::atomic<uint32_t> generation_{0}, cdc_generation_{0}, midi_generation_{0};
    bool suspended_ = false;
    uint16_t control_lines_ = 0;
    bool line_pending_ = false, cdc_zlp_ = false, cdc_zlp_pending_ = false;
    bool notify_busy_ = false, notify_dirty_ = false;
};
}
