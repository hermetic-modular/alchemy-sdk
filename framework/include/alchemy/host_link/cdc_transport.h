#pragma once
#include <atomic>
#include "alchemy/host_link/transport.h"
#include "alchemy/host_link/wire.h"
#include "alchemy/usb/device.h"

namespace alchemy::hostlink {
namespace detail {
constexpr size_t CdcRingPow2(size_t value) {
    size_t result = 2; while (result < value) result <<= 1; return result;
}
}

class CdcUsbTransport : public IHostTransport {
    struct Packet {
        std::array<uint8_t,64> bytes{};
        uint32_t generation = 0;
        uint8_t size = 0;
    };
    static constexpr size_t kRxPackets = detail::CdcRingPow2((kMaxWire * 3 + 63) / 64);
  public:
    ~CdcUsbTransport() override;
    // Explicit product must match shared USB; fallback applies only to a new identity.
    bool Init(daisy::UsbHandle::UsbPeriph peripheral = daisy::UsbHandle::FS_EXTERNAL,
              const char* product = nullptr, const char* fallback_product = nullptr);
    size_t Read(uint8_t* destination, size_t max) override;
    size_t Write(const uint8_t* source, size_t size) override;
    size_t WriteSpace() const override;
    uint32_t ReadGeneration() const override { return read_generation_; }
    void Pump() override;
    uint32_t RxDropped() const { return dropped_.load(std::memory_order_relaxed); }
  private:
    void OnRx(const uint8_t* bytes,size_t size);
    usb::Device* device_ = nullptr;
    midi::Queue<Packet,kRxPackets> receive_;
    Packet packet_{};
    size_t position_ = 0;
    uint32_t read_generation_ = 0;
    std::atomic<uint32_t> dropped_{0};
};
}
