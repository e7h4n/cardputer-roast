/*
 * Roaster link: Modbus RTU over the E-Roast BLE UART module (service FFE0 / characteristic FFE1).
 *
 * Protocol recovered from myERoast 1.0.17 / 1.0.18 (identical): slave 1, FC03 reads of 7 registers,
 * FC06 single-register writes acknowledged by an echo frame. Values are 0.1 units (100 % == 1000).
 *
 *   reg 2  roast timer (s)            reg 4  bean temperature BT (x10)
 *   reg 9  PID target / curve SV (x10) reg 11 heater output (x10 %)
 *   reg 12 fan setpoint (x10 %)        reg 13 =1 start (fan on)
 *   reg 14 =2 drop, =1 stop             reg 15 PID on/off (1/0)
 *   reg 16 fan output (x10 %)          reg 20 PID cycle ms, 21/22/23 P/I/D
 *   reg 24 fan delay, reg 28 min fan, reg 29 max heater
 *
 * The firmware-update (ISP) path of the vendor app is deliberately not implemented.
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

namespace roast {

constexpr int kRegCount = 30;

enum class LinkState : uint8_t {
    Off,
    Scanning,
    Connecting,
    Ready,
    Lost,  // was ready, connection dropped; reconnect in progress
    Error,
};

struct RegSnapshot {
    int16_t reg[kRegCount] = {0};
    uint32_t valid         = 0;  // bit i set when reg i has been read at least once
    uint32_t lastRxMs      = 0;  // millis() of the last valid frame
    uint32_t frames        = 0;  // valid read responses received

    bool has(int i) const
    {
        return (valid >> i) & 1u;
    }
    float bt() const
    {
        return reg[4] / 10.0f;
    }
    float target() const
    {
        return reg[9] / 10.0f;
    }
    float heat() const
    {
        return reg[11] / 10.0f;
    }
    float fan() const
    {
        return reg[12] / 10.0f;
    }
    float fanOut() const
    {
        return reg[16] / 10.0f;
    }
    bool pidOn() const
    {
        return reg[15] == 1;
    }
};

enum class TxStatus : uint8_t { Pending, Ok, Failed };

struct ScanEntry {
    std::string name;
    int rssi = -127;
};

using RegWrite = std::pair<uint16_t, uint16_t>;  // address, raw value

class RoasterLink {
public:
    virtual ~RoasterLink() = default;

    /// Start scanning / connecting. Safe to call repeatedly.
    virtual bool begin() = 0;
    /// Disconnect and release the radio.
    virtual void end() = 0;

    virtual LinkState state() = 0;
    virtual std::string deviceName() = 0;
    virtual std::string lastError() = 0;
    virtual std::vector<ScanEntry> scanResults() = 0;
    virtual bool isSimulated() const = 0;

    /// Copy of the register cache.
    virtual RegSnapshot snapshot() = 0;

    /// Queue an ordered command (writes executed in order, each confirmed by echo).
    /// Returns a transaction id for txStatus().
    virtual uint32_t command(const std::vector<RegWrite>& writes) = 0;
    virtual TxStatus txStatus(uint32_t id) = 0;

    /// Latest-value slot for the PID target (reg 9). Older unsent values are replaced.
    virtual void setTarget(uint16_t raw) = 0;
    /// Latest-value slot for the heater output (reg 11), written once a second by the Cardputer PID.
    virtual void setHeat(uint16_t raw) = 0;
    /// Forget a heater value not yet written (before a drop: it must not land after it).
    virtual void cancelHeat() {}

    /// Drop anything still queued (used when leaving a roast).
    virtual void clearQueue() = 0;
};

RoasterLink* CreateBleLink();
RoasterLink* CreateSimLink();

}  // namespace roast
