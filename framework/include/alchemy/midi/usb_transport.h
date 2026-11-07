#pragma once
#include <algorithm>
#include "alchemy/midi/receiver.h"
#include "alchemy/midi/usb_packets.h"
#include "alchemy/usb/device.h"
#include "sys/system.h"
#include "util/scopedirqblocker.h"

namespace alchemy::midi {
template<size_t ByteCapacity = 1024> class DaisyUsbPort {
    struct ReceivedPacket { UsbPacket packet{}; uint32_t tick = 0, generation = 0; };
    static constexpr size_t kMaxBundle = usb::kPacketSize / 4;
    static_assert(ByteCapacity >= 8, "USB receive capacity is too small");
  public:
    struct Config {
        daisy::UsbHandle::UsbPeriph peripheral = daisy::UsbHandle::FS_EXTERNAL;
        const char* product = nullptr;
        bool recover_faults = true;
    };
    ~DaisyUsbPort() { if (device_) device_->SetMidiReceiver({this,nullptr}); }
    bool Init(Config config, SourceId source) {
        if (device_ || source.port == kAnyPort || !source.session || !daisy::System::GetTickFreq()) return false;
        auto& device = alchemy::usb::Device::Shared();
        if (!device.Init(config.peripheral,config.product)) return false;
        daisy::ScopedIrqBlocker guard;
        recover_faults_ = config.recover_faults; sysex_ = UsbSysExEncoder();
        device_ = &device;
        if (!device.SetMidiReceiver({this,ReceiveCallback})) { device_ = nullptr; return false; }
        if (!receiver_.Init(source,daisy::System::GetTickFreq())) { device.SetMidiReceiver({this,nullptr}); device_ = nullptr; return false; }
        generation_ = device.Generation(); return true;
    }
    bool Restart(SourceId source) {
        // Decoder reset may invoke the application's SysEx callback.
        if (!device_ || !receiver_.Restart(source)) return false;
        daisy::ScopedIrqBlocker guard;
        packets_.Clear(); overflow_.store(false,std::memory_order_release);
        packet_decoder_.Reset(); generation_ = device_->Generation(); disconnected_ = false; return true;
    }
    template<class Sink> size_t Poll(TickTimebase::Sample now, Sink&& sink) {
        if (!device_) return 0;
        device_->Poll();
        SyncOutputGeneration();
        size_t delivered = 0;
        if (!device_->Available() || generation_ != device_->Generation()) {
            if (!disconnected_) { receiver_.MarkLost(); disconnected_ = true; }
            delivered += receiver_.Poll(now,sink);
            if (!device_->Available() || !receiver_.ResetReported()) return delivered;
            // Keep queued packets from the new bus generation across this restart.
            receiver_.Restart(NextSession());
            packet_decoder_.Reset();
            generation_ = device_->Generation();
            disconnected_ = false;
        }
        {
            daisy::ScopedIrqBlocker guard;
            if (overflow_.exchange(false,std::memory_order_acq_rel) && overflow_generation_ == generation_) receiver_.MarkLost();
        }
        if (receiver_.Faulted()) {
            delivered += receiver_.Poll(now,sink);
            if (recover_faults_ && receiver_.ResetReported()) Restart(NextSession());
            return delivered;
        }
        ReceivedPacket record;
        for (size_t i = 0; i < ByteCapacity / 4 && packets_.Pop(record); ++i) {
            if (record.generation != generation_) continue;
            uint8_t cable; const uint8_t* decoded; size_t count;
            if (record.packet.bytes[0] >> 4 != 0) continue;
            if (!packet_decoder_.Decode(record.packet,cable,decoded,count)) { receiver_.MarkLost(); break; }
            receiver_.Receive(decoded,count,record.tick);
        }
        return delivered + receiver_.Poll(now,sink);
    }
    uint32_t DestinationGeneration() const { return device_ ? device_->MidiGeneration() : 0; }
    bool Available() const { return device_ && device_->Available(); }
    void MarkLost() { receiver_.MarkLost(); }
    TxResult Submit(const ShortMessage& message) { return Submit(message,DestinationGeneration()); }
    TxResult Submit(const ShortMessage& message,uint32_t generation) {
        if (!device_) return TxResult::Disconnected;
        if (!CanTransmitHere()) return TxResult::Invalid;
        device_->Refresh();
        if (SyncOutputGeneration() != generation || !device_->Available()) return TxResult::Disconnected;
        if (sysex_.Active() && message.bytes[0] < 0xf8) return TxResult::Busy;
        UsbPacket packet;
        const auto result = EncodeUsb(message,packet,0);
        return result == TxResult::Accepted ? SubmitPacket(packet,generation) : result;
    }
    TxResult SubmitBundle(const ShortMessage* messages,size_t count,uint32_t generation) {
        if (!device_) return TxResult::Disconnected;
        if (!CanTransmitHere() || !messages || !count || count > kMaxBundle) return TxResult::Invalid;
        device_->Refresh();
        if (SyncOutputGeneration() != generation || !device_->Available()) return TxResult::Disconnected;
        std::array<uint8_t, usb::kPacketSize> bytes{};
        for (size_t i = 0; i < count; ++i) {
            if (sysex_.Active() && messages[i].bytes[0] < 0xf8) return TxResult::Busy;
            UsbPacket packet;
            const auto result = EncodeUsb(messages[i],packet,0);
            if (result != TxResult::Accepted) return result;
            std::copy(packet.bytes.begin(),packet.bytes.end(),bytes.begin()+4*i);
        }
        return SubmitPackets(bytes.data(), 4 * count, false, generation);
    }
    struct StreamWrite { TxResult result = TxResult::Accepted; size_t accepted = 0; };
    StreamWrite WriteSysEx(const uint8_t* bytes,size_t size,size_t budget = 256) {
        if (!bytes && size) return {TxResult::Invalid,0};
        StreamWrite result;
        for (; result.accepted < size && result.accepted < budget; ++result.accepted) {
            result.result = SendSysExByte(bytes[result.accepted]);
            if (result.result != TxResult::Accepted) break;
        }
        return result;
    }
    TxResult SendSysExByte(uint8_t byte) {
        if (!device_) return TxResult::Disconnected;
        if (!CanTransmitHere()) return TxResult::Invalid;
        device_->Refresh();
        const auto generation = SyncOutputGeneration();
        if (!device_->Available()) return TxResult::Disconnected;
        if (sysex_cancelled_) {
            if (byte == 0xf0) sysex_cancelled_ = false;
            else if (byte < 0xf8) return TxResult::Disconnected;
        }
        return sysex_.Feed(byte,[&](const UsbPacket& packet) { return SubmitPacket(packet,generation); });
    }
    OutputSink Sink() { return {this,[](void* p,const ShortMessage& message,uint32_t generation) {
        return static_cast<DaisyUsbPort*>(p)->Submit(message,generation);
    },16,[](void* p,const ShortMessage* messages,size_t count,uint32_t generation) {
        return static_cast<DaisyUsbPort*>(p)->SubmitBundle(messages,count,generation);
    },[](void* p) { return static_cast<DaisyUsbPort*>(p)->DestinationGeneration(); },kMaxBundle}; }
    void SetActivity(InputActivity observer) { receiver_.SetActivity(observer); }
    void SetSysExSink(SysExSink observer) { receiver_.SetSysExSink(observer); }
    bool HasActivity() const { return receiver_.HasActivity(); }
    uint32_t LastActivityUs() const { return receiver_.LastActivityUs(); }
    bool Faulted() const { return receiver_.Faulted(); }
    SourceId Source() const { return receiver_.Source(); }
    uint32_t CompletedTransfers() const { return device_ ? device_->MidiCompleted() : 0; }
    uint32_t CancelledTransfers() const { return device_ ? device_->MidiCancelled() : 0; }
    TransportCapabilities Capabilities() const {
        TransportCapabilities result;
        result.transmit = result.observable_tx_acceptance = result.realtime_transmit = result.system_common_transmit = true;
        return result;
    }
  private:
    SourceId NextSession() const {
        auto source = receiver_.Source();
        if (++source.session == 0) ++source.session;
        return source;
    }
    static bool CanTransmitHere() { return !__get_IPSR() && !__get_PRIMASK() && !__get_BASEPRI(); }
    uint32_t SyncOutputGeneration() {
        const auto generation = DestinationGeneration();
        if (sysex_generation_ != generation) {
            sysex_cancelled_ = sysex_cancelled_ || sysex_.Active();
            sysex_.Reset(); sysex_generation_ = generation;
        }
        return generation;
    }
    TxResult SubmitPacket(const UsbPacket& packet,uint32_t generation) {
        return SubmitPackets(packet.bytes.data(), packet.bytes.size(), (packet.bytes[0] & 15) == 15, generation);
    }
    TxResult SubmitPackets(const uint8_t* bytes, size_t size, bool realtime, uint32_t generation) {
        if (device_->SendMidi(bytes, size, realtime, generation)) return TxResult::Accepted;
        if (!device_->Available() || DestinationGeneration() != generation) return TxResult::Disconnected;
        return TxResult::Busy;
    }
    static void ReceiveCallback(void* context,const uint8_t* bytes,size_t size) {
        auto& self = *static_cast<DaisyUsbPort*>(context);
        if (size % 4) { self.overflow_generation_ = self.device_->Generation(); self.overflow_.store(true,std::memory_order_release); return; }
        ReceivedPacket record; record.tick = daisy::System::GetTick(); record.generation = self.device_->Generation();
        for (size_t i = 0; i < size; i += 4) {
            for (size_t j = 0; j < 4; ++j) record.packet.bytes[j] = bytes[i + j];
            if (!self.packets_.Push(record)) { self.overflow_generation_ = record.generation; self.overflow_.store(true,std::memory_order_release); return; }
        }
    }
    alchemy::usb::Device* device_ = nullptr;
    Receiver<ByteCapacity> receiver_;
    Queue<ReceivedPacket,ByteCapacity/4> packets_;
    std::atomic<bool> overflow_{false};
    UsbSysExEncoder sysex_{};
    UsbPacketDecoder packet_decoder_{};
    uint32_t generation_ = 0, sysex_generation_ = 0, overflow_generation_ = 0;
    bool disconnected_ = false, sysex_cancelled_ = false, recover_faults_ = true;
};
}
