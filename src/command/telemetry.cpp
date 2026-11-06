#include "command/telemetry.h"

#include <algorithm>

#include "esp_random.h"

namespace command {

namespace {

// Simulated temperature: random walk inside a plausible outdoor range.
constexpr float kTempMinC = 18.0f;
constexpr float kTempMaxC = 32.0f;
constexpr float kTempStepC = 0.3f;

// Simulated battery: slow linear drain from full, plus a little noise.
constexpr float kBatteryFullV = 4.10f;
constexpr float kBatteryMinV = 3.50f;
constexpr float kBatteryMaxV = 4.20f;
constexpr float kBatteryDrainVPerHour = 0.05f;
constexpr float kBatteryNoiseV = 0.02f;

float g_tempC = 23.0f;

// Uniform random value in [-1, 1].
float noise() { return float(esp_random()) / 4294967295.0f * 2.0f - 1.0f; }

}  // namespace

Telemetry sampleTelemetry(api::RadioApi& api, const protocol::RxInfo& request) {
    const api::Status s = api.status();

    Telemetry t{};
    t.uptimeS = s.uptimeMs / 1000;
    t.rxPackets = s.rxPackets;
    t.txPackets = s.txPackets;
    t.frequencyHz = s.frequencyHz;
    t.rssiDbm = request.rssiDbm;
    t.rssiValid = request.rssiValid;

    g_tempC = std::clamp(g_tempC + noise() * kTempStepC, kTempMinC, kTempMaxC);
    t.temperatureC = g_tempC;

    const float hours = float(s.uptimeMs) / 3600000.0f;
    t.batteryV = std::clamp(kBatteryFullV - hours * kBatteryDrainVPerHour + noise() * kBatteryNoiseV,
                            kBatteryMinV, kBatteryMaxV);
    return t;
}

}  // namespace command
