#pragma once

#include <cstddef>
#include <cstdint>

namespace util {

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF). Software check used by the
// stream header and the stress ping; independent of the CC1120's hardware CRC.
inline uint16_t crc16(const void* data, size_t len, uint16_t crc = 0xFFFF) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        crc ^= uint16_t(p[i]) << 8;
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
        }
    }
    return crc;
}

}  // namespace util
