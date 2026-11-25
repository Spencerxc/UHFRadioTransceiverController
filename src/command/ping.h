#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

#include "api/radio_api.h"

namespace command {

// Stress-test ping.
//
// Request:  PING::::id=<n>;ck=<crc16 hex of data>[;f=d|c];d=<data>
// Reply:    RSP::PONG::id=<n>;ul=ok|crc|data;rs=<dBm|NA>;d=<echoed data>
//
// The remote checks the request (ul=crc: it failed the radio CRC; ul=data: the
// data does not match ck) and echoes the data, so the sender can tell which way
// a packet was damaged. `f` asks the remote to inject a fault: d = do not reply
// (simulated drop), c = corrupt one character of the echo. A plain PING without
// a `d` field still gets the short RSP::PONG::id=<n> reply.
constexpr size_t kMaxPingData = 80;  // keeps the reply within one frame

enum class PingFault : uint8_t {
    None,
    Drop,         // remote does not reply
    CorruptUp,    // sender alters one data character after computing ck
    CorruptDown,  // remote alters one character of the echo
};

enum class PingOutcome : uint8_t {
    Ok,
    Lost,         // no reply before the timeout
    CorruptUp,    // request damaged on the way to the remote
    CorruptDown,  // reply damaged on the way back
    Error,        // transmit failed or the reply was not a PONG
};

struct PingResult {
    PingOutcome outcome;
    const char* detail;        // reason for a non-Ok outcome, "" if Ok
    bool replied;              // a reply with the right id arrived
    uint32_t rttMs;            // valid if replied
    protocol::RxInfo reply;    // link quality of the reply, measured here (valid if replied)
    bool remoteRssiValid;
    int16_t remoteRssiDbm;     // RSSI of the request, measured by the remote
    uint16_t late;             // replies to earlier pings discarded while waiting
};

// Sends one stress ping carrying dataLen (0..kMaxPingData) random characters and
// classifies the result. Returns ESP_ERR_INVALID_ARG for a bad length, or the
// transmit error; otherwise ESP_OK with the outcome in `out`.
esp_err_t ping(api::RadioApi& api, size_t dataLen, PingFault fault, uint32_t timeoutMs,
               PingResult& out);

const char* outcomeName(PingOutcome o);

// Replaces one random character of s[0..len) with a different alphanumeric one.
void corruptText(char* s, size_t len);

}  // namespace command
