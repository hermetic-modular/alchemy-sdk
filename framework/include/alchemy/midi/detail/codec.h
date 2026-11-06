#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace alchemy::midi::detail {

class Codec {
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
                  "MIDI persistence requires binary32 floats");
  protected:
    static void Put16(uint8_t* p, uint16_t value) {
        p[0] = static_cast<uint8_t>(value); p[1] = static_cast<uint8_t>(value >> 8);
    }
    static uint16_t Get16(const uint8_t* p) {
        return static_cast<uint16_t>(p[0] | (uint16_t{p[1]} << 8));
    }
    static void Put32(uint8_t* p, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (8 * i));
    }
    static uint32_t Get32(const uint8_t* p) {
        return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24);
    }
    static void PutFloat(uint8_t* p, float value) {
        uint32_t bits; std::memcpy(&bits, &value, sizeof(bits)); Put32(p, bits);
    }
    static float GetFloat(const uint8_t* p) {
        const uint32_t bits = Get32(p); float value; std::memcpy(&value, &bits, sizeof(value)); return value;
    }
    static uint32_t Crc(const uint8_t* bytes, size_t size, size_t checksum_offset) {
        uint32_t crc = 0xffffffffu;
        for (size_t i = 0; i < size; ++i) {
            crc ^= i >= checksum_offset && i - checksum_offset < 4 ? uint8_t{0} : bytes[i];
            for (unsigned bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (0xedb88320u & (uint32_t{0} - (crc & 1u)));
        }
        return ~crc;
    }
};

}
