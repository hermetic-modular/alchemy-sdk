#pragma once
#include <algorithm>
#include "alchemy/midi/output.h"

namespace alchemy::midi {
struct UsbPacket { std::array<uint8_t, 4> bytes{}; };

inline TxResult EncodeUsb(const ShortMessage& message, UsbPacket& packet, uint8_t cable = 0) {
    packet = {};
    if (cable > 15) return TxResult::Invalid;
    const auto result = Validate(message);
    if (result != TxResult::Accepted) return result;
    const auto status = message.bytes[0];
    uint8_t cin = status >> 4;
    if (status >= 0xf0 && status < 0xf8) cin = message.size == 1 ? 5 : message.size;
    packet.bytes[0] = uint8_t(cable << 4 | cin);
    for (uint8_t i = 0; i < message.size; ++i) packet.bytes[i + 1] = message.bytes[i];
    return TxResult::Accepted;
}

inline bool DecodeUsb(const UsbPacket& packet, uint8_t& cable, const uint8_t*& bytes, size_t& count) {
    constexpr uint8_t sizes[16] = {0,0,2,3,3,1,2,3,3,3,3,3,2,2,3,1};
    const uint8_t cin = packet.bytes[0] & 15;
    cable = packet.bytes[0] >> 4;
    count = sizes[cin];
    bytes = packet.bytes.data() + 1;
    if (!count) return false;
    const uint8_t status = bytes[0];
    size_t first_data = 1, end_data = count;
    switch (cin) {
    case 2:
        if (status != 0xf1 && status != 0xf3) return false;
        break;
    case 3:
        if (status != 0xf2) return false;
        break;
    case 4: case 6: case 7:
        first_data = status == 0xf0 ? 1 : 0;
        if (cin != 4) {
            --end_data;
            if (bytes[end_data] != 0xf7) return false;
        }
        break;
    case 5: return status == 0xf6 || status == 0xf7;
    case 15: return true;
    default:
        if ((status >> 4) != cin) return false;
    }
    return std::all_of(bytes + first_data, bytes + end_data, [](uint8_t byte) { return byte < 0x80; });
}

// CIN 0xF carries raw MIDI bytes and participates in the same SysEx stream.
class UsbPacketDecoder {
  public:
    bool Decode(const UsbPacket& packet, uint8_t& cable, const uint8_t*& bytes, size_t& count) {
        if (!DecodeUsb(packet,cable,bytes,count)) return false;
        const auto cin = packet.bytes[0] & 15;
        bool active = sysex_[cable];
        if (cin == 4 || (cin >= 5 && cin <= 7 && bytes[0] != 0xf6)) {
            if (bytes[0] != 0xf0 && !active) return false;
            active = cin == 4;
        } else if (cin == 15) {
            if (bytes[0] == 0xf0) active = true;
            else if (bytes[0] >= 0x80 && bytes[0] < 0xf8) active = false;
        } else if (bytes[0] < 0xf8) active = false;
        sysex_[cable] = active;
        return true;
    }
    void Reset() { sysex_.fill(false); }
  private:
    std::array<bool,16> sysex_{};
};

class UsbSysExEncoder {
  public:
    explicit UsbSysExEncoder(uint8_t cable = 0) : cable_(cable) {}
    template<class Sink> TxResult Feed(uint8_t byte, Sink&& sink) {
        if (cable_ > 15) return TxResult::Invalid;
        if (byte >= 0xf8) {
            UsbPacket packet{{uint8_t(cable_ << 4 | 15), byte, 0, 0}};
            return sink(packet);
        }
        auto next = *this;
        if (byte == 0xf0) {
            if (active_) return TxResult::Invalid;
            next.active_ = true; next.count_ = 0;
        } else if (!active_ || (byte >= 128 && byte != 0xf7)) return TxResult::Invalid;
        next.pending_[next.count_++] = byte;
        if (next.count_ == 3 || byte == 0xf7) {
            UsbPacket packet;
            packet.bytes[0] = uint8_t(cable_ << 4 | (byte == 0xf7 ? 4 + next.count_ : 4));
            for (uint8_t i = 0; i < next.count_; ++i) packet.bytes[i + 1] = next.pending_[i];
            const auto result = sink(packet);
            if (result != TxResult::Accepted) return result;
            next.count_ = 0;
        }
        if (byte == 0xf7) next.active_ = false;
        *this = next;
        return TxResult::Accepted;
    }
    void Reset() { count_ = 0; active_ = false; }
    bool Active() const { return active_; }
  private:
    std::array<uint8_t, 3> pending_{};
    uint8_t cable_ = 0, count_ = 0;
    bool active_ = false;
};
}
