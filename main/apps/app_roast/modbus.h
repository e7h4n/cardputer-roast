/*
 * Minimal Modbus RTU helpers (CRC16/MODBUS, FC03/FC06 framing, stream resync).
 */
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

namespace roast {
namespace modbus {

inline uint16_t crc16(const uint8_t* d, size_t n)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

inline void put_crc(uint8_t* d, size_t n)
{
    uint16_t c = crc16(d, n);
    d[n]       = c & 0xFF;
    d[n + 1]   = c >> 8;
}

inline void build_read(uint8_t slave, uint16_t start, uint16_t qty, uint8_t out[8])
{
    out[0] = slave;
    out[1] = 0x03;
    out[2] = start >> 8;
    out[3] = start & 0xFF;
    out[4] = qty >> 8;
    out[5] = qty & 0xFF;
    put_crc(out, 6);
}

inline void build_write_single(uint8_t slave, uint16_t addr, uint16_t value, uint8_t out[8])
{
    out[0] = slave;
    out[1] = 0x06;
    out[2] = addr >> 8;
    out[3] = addr & 0xFF;
    out[4] = value >> 8;
    out[5] = value & 0xFF;
    put_crc(out, 6);
}

/// Expected length of the frame at the head of `b`, 0 if unknown yet, -1 if the head byte cannot start a frame.
inline int expected_len(const std::vector<uint8_t>& b, uint8_t slave = 1)
{
    if (b.empty()) return 0;
    if (b[0] != slave) return -1;
    if (b.size() < 3) return 0;
    uint8_t fn = b[1];
    if (fn & 0x80) return 5;
    if (fn == 0x03) {
        if (b[2] == 0 || b[2] > 120) return -1;
        return 3 + b[2] + 2;
    }
    if (fn == 0x06 || fn == 0x10) return 8;
    return -1;
}

/// Pull one CRC-valid frame out of the receive buffer. Garbage is discarded one byte at a time.
/// Leaves `frame` untouched and returns false when no complete frame is available.
inline bool extract_frame(std::vector<uint8_t>& b, std::vector<uint8_t>& frame, uint8_t slave = 1)
{
    while (!b.empty()) {
        int n = expected_len(b, slave);
        if (n < 0) {
            b.erase(b.begin());
            continue;
        }
        if (n == 0 || (int)b.size() < n) return false;
        uint16_t c = crc16(b.data(), n - 2);
        if ((c & 0xFF) == b[n - 2] && (c >> 8) == b[n - 1]) {
            frame.assign(b.begin(), b.begin() + n);
            b.erase(b.begin(), b.begin() + n);
            return true;
        }
        b.erase(b.begin());
    }
    return false;
}

}  // namespace modbus
}  // namespace roast
