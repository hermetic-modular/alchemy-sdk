#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include "alchemy/midi/receiver.h"
#include "alchemy/midi/types.h"
#include "alchemy/midi/output.h"
#include "per/uart.h"
#include "sys/system.h"
#include "sys/dma.h"
#include "util/scopedirqblocker.h"

namespace alchemy::midi {

class UartMidiResources {
  public:
    UartMidiResources() = default;
    UartMidiResources(const UartMidiResources&) = delete;
    UartMidiResources& operator=(const UartMidiResources&) = delete;
  private:
    template<size_t> friend class DaisyUartPort;
    bool Acquire() { if (claimed_) return false; claimed_ = true; owned_ = true; return true; }
    void Release() { if (owned_) { claimed_ = false; owned_ = false; } }
    // Stock DMA ownership remains claimed until firmware reset.
    inline static bool claimed_ = false;
    bool owned_ = false;
};

template<size_t ByteCapacity = 256> class DaisyUartPort {
  public:
    ~DaisyUartPort() {
        if (initialized_) uart_.DmaListenStop();
    }
    struct Config {
        daisy::UartHandler::Config uart;
        // Noncached DMA memory, 8..16 bytes.
        uint8_t* rx_dma_buffer = nullptr;
        size_t rx_dma_size = 0;
        UartMidiResources* resources = nullptr;
        bool dma_in_use = false;
    };
    bool Init(Config config, SourceId source) {
        const uint32_t frequency = daisy::System::GetTickFreq();
        if (initialized_ || !config.resources || config.dma_in_use ||
            !frequency || source.port == kAnyPort || !source.session ||
            !config.rx_dma_buffer || config.rx_dma_size < 8 || config.rx_dma_size > 16 || config.rx_dma_size > ByteCapacity || (config.rx_dma_size & 1u) ||
            static_cast<unsigned>(config.uart.periph) > 7) return false;
        config.uart.baudrate = 31250;
        config.uart.stopbits = daisy::UartHandler::Config::StopBits::BITS_1;
        config.uart.parity = daisy::UartHandler::Config::Parity::NONE;
        config.uart.wordlength = daisy::UartHandler::Config::WordLength::BITS_8;
        config.uart.mode = daisy::UartHandler::Config::Mode::RX;
        if (!config.resources->Acquire()) return false;
        config_ = config;
        if (uart_.Init(config.uart) != daisy::UartHandler::Result::OK) { config.resources->Release(); return false; }
        if (!receiver_.Init(source, frequency)) { config.resources->Release(); return false; }
        initialized_ = true;
        dsy_dma_clear_cache_for_buffer(config_.rx_dma_buffer, config_.rx_dma_size);
        if (uart_.DmaListenStart(config_.rx_dma_buffer, config_.rx_dma_size,
                                 ReceiveCallback, this) != daisy::UartHandler::Result::OK) {
            uart_.DmaListenStop(); receiver_.MarkLost(); return false;
        }
        return true;
    }
    bool Restart(SourceId source) {
        if (!initialized_ || !source.session || source.port != receiver_.Source().port ||
            !SessionAfter(source.session, receiver_.Source().session)) return false;
        if (uart_.DmaListenStop() != daisy::UartHandler::Result::OK) return false;
        if (!receiver_.Restart(source)) return false;
        if (uart_.DmaListenStart(config_.rx_dma_buffer, config_.rx_dma_size,
                                 ReceiveCallback, this) != daisy::UartHandler::Result::OK) {
            receiver_.MarkLost(); return false;
        }
        return true;
    }
    template<class Sink> size_t Poll(TickTimebase::Sample now, Sink&& sink) {
        if (initialized_ && !uart_.IsListening()) receiver_.MarkLost();
        return receiver_.Poll(now, sink);
    }
    void MarkLost() { receiver_.MarkLost(); }
    void SetActivity(InputActivity observer) { receiver_.SetActivity(observer); }
    void SetSysExSink(SysExSink observer) { receiver_.SetSysExSink(observer); }
    bool HasActivity() const { return receiver_.HasActivity(); }
    uint32_t LastActivityUs() const { return receiver_.LastActivityUs(); }
    bool Faulted() const { return receiver_.Faulted(); }
    SourceId Source() const { return receiver_.Source(); }
    TransportCapabilities Capabilities() const { return {}; }
  private:
    static void ReceiveCallback(uint8_t* bytes, size_t size, void* context,
                                  daisy::UartHandler::Result result) {
        auto& self = *static_cast<DaisyUartPort*>(context);
        if (result != daisy::UartHandler::Result::OK) { self.receiver_.MarkLost(); return; }
        dsy_dma_invalidate_cache_for_buffer(bytes, size);
        self.receiver_.Receive(bytes, size, daisy::System::GetTick());
    }
    Config config_{};
    daisy::UartHandler uart_;
    Receiver<ByteCapacity> receiver_;
    bool initialized_ = false;
};

}

#include "alchemy/midi/usb_transport.h"
