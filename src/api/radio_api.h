#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

#include "protocol/protocol_manager.h"

namespace api {

struct Status {
    bool ready;                // radio found and configured
    protocol::State state;     // Rx unless a transmit is in progress
    uint32_t frequencyHz;      // 0 if it could not be read
    uint32_t rxPackets;        // packets received since boot
    uint32_t txPackets;        // packets transmitted since boot
    uint32_t uptimeMs;         // time since boot
};

// Interface API (1U1SMGR2).
//
// The public surface of Radio Unit 1. Applications call these functions; they
// are forwarded to the Protocol Process Manager and never touch the SPI driver
// or the RF switch directly. The unit listens (RX) by default and returns to RX
// after every transmit.
class RadioApi {
public:
    explicit RadioApi(protocol::ProcessManager& manager) : manager_(manager) {}

    // Transmits one packet of 1..protocol::kMaxPayload bytes.
    esp_err_t send(const uint8_t* data, size_t len) { return manager_.transmit(data, len); }

    // Waits up to timeoutMs for a received packet. If info is not null it gets the
    // packet's RSSI (dBm), LQI and CRC status.
    esp_err_t receive(uint8_t* buf, size_t bufLen, size_t& outLen, uint32_t timeoutMs,
                      protocol::RxInfo* info = nullptr) {
        return manager_.receive(buf, bufLen, outLen, timeoutMs, info);
    }

    esp_err_t setFrequency(uint32_t hz) { return manager_.setFrequency(hz); }

    Status status();

private:
    protocol::ProcessManager& manager_;
};

}  // namespace api
