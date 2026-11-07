#pragma once
#include <cstdint>

namespace alchemy::usb {
inline constexpr uint8_t kCdcOut = 0x01, kCdcIn = 0x81, kCdcNotify = 0x82;
inline constexpr uint8_t kMidiOut = 0x03, kMidiIn = 0x83;
inline constexpr uint16_t kPacketSize = 64;
inline constexpr uint8_t kConfiguration[] = {
    9,2,175,0,4,1,0,0xc0,50,
    8,11,0,2,2,2,1,4,
    9,4,0,0,1,2,2,1,4,
    5,0x24,0,0x10,1,
    5,0x24,1,0,1,
    4,0x24,2,2,
    5,0x24,6,0,1,
    7,5,kCdcNotify,3,8,0,16,
    9,4,1,0,2,10,0,0,4,
    7,5,kCdcOut,2,kPacketSize,0,0,
    7,5,kCdcIn,2,kPacketSize,0,0,
    8,11,2,2,1,1,0,5,
    9,4,2,0,0,1,1,0,5,
    9,0x24,1,0,1,9,0,1,3,
    9,4,3,0,2,1,3,0,5,
    7,0x24,1,0,1,65,0,
    6,0x24,2,1,1,0,
    6,0x24,2,2,2,0,
    9,0x24,3,1,3,1,2,1,0,
    9,0x24,3,2,4,1,1,1,0,
    9,5,kMidiOut,2,kPacketSize,0,0,0,0,
    5,0x25,1,1,1,
    9,5,kMidiIn,2,kPacketSize,0,0,0,0,
    5,0x25,1,1,3
};
static_assert(sizeof(kConfiguration) == 175, "Composite descriptor length mismatch");
}
