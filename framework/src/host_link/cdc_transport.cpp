#include "alchemy/host_link/cdc_transport.h"
#include <algorithm>
#include <cstring>

namespace alchemy::hostlink {
CdcUsbTransport::~CdcUsbTransport() { if (device_) device_->SetCdcReceiver({this,nullptr}); }
bool CdcUsbTransport::Init(daisy::UsbHandle::UsbPeriph peripheral,
                          const char* product, const char* fallback_product) {
    auto& device = usb::Device::Shared();
    if (!device.Init(peripheral,product,fallback_product)) return false;
    device_ = &device;
    if (!device.SetCdcReceiver({this,[](void* context,const uint8_t* bytes,size_t size) {
        static_cast<CdcUsbTransport*>(context)->OnRx(bytes,size);
    }})) { device_ = nullptr; return false; }
    return true;
}
void CdcUsbTransport::OnRx(const uint8_t* bytes,size_t size) {
    if (!device_ || !bytes || size > 64) return;
    Packet packet; packet.generation = device_->CdcGeneration(); packet.size = uint8_t(size);
    std::memcpy(packet.bytes.data(),bytes,size);
    if (!receive_.Push(packet)) dropped_.fetch_add(uint32_t(size),std::memory_order_relaxed);
}
size_t CdcUsbTransport::Read(uint8_t* destination,size_t max) {
    if (!device_ || !destination) return 0;
    device_->Refresh();
    if (!device_->Available()) return 0;
    size_t count = 0, processed = 0;
    while (count < max) {
        if (position_ == packet_.size || packet_.generation != device_->CdcGeneration()) {
            if (processed++ == kRxPackets || !receive_.Pop(packet_)) break;
            position_ = 0;
            if (packet_.generation != device_->CdcGeneration()) { position_ = packet_.size; continue; }
        }
        if (count && packet_.generation != read_generation_) break;
        read_generation_ = packet_.generation;
        const auto size = std::min(max-count,size_t(packet_.size)-position_);
        std::memcpy(destination+count,packet_.bytes.data()+position_,size);
        count+=size; position_+=size;
    }
    return count;
}
size_t CdcUsbTransport::WriteSpace() const { return device_ ? device_->CdcSpace() : 0; }
size_t CdcUsbTransport::Write(const uint8_t* source,size_t size) {
    if (!device_ || !source) return 0;
    size_t accepted=0;
    while (accepted<size) {
        const auto chunk=std::min(size-accepted,size_t(256));
        if (!device_->SendCdc(source+accepted,chunk)) break;
        accepted+=chunk;
    }
    return accepted;
}
void CdcUsbTransport::Pump() { if (device_) device_->Poll(); }
}
