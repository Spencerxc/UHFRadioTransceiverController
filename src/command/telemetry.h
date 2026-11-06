#pragma once

#include <cstdint>

#include "api/radio_api.h"

namespace command {

struct Telemetry {
    uint32_t uptimeS;      // real
    uint32_t rxPackets;    // real
    uint32_t txPackets;    // real
    uint32_t frequencyHz;  // real (0 if unreadable)
    int16_t rssiDbm;       // real: RSSI of the request this telemetry answers
    bool rssiValid;
    float temperatureC;    // SIMULATED: no sensor fitted
    float batteryV;        // SIMULATED: no sensor fitted
};

// Takes a telemetry sample. Radio values come from the Interface API, and the
// RSSI from `request` (the received packet being answered). Temperature and
// battery voltage are simulated until real sensors are attached.
// Not thread-safe: call from one task only (the command server).
Telemetry sampleTelemetry(api::RadioApi& api, const protocol::RxInfo& request);

}  // namespace command
