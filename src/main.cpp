#include "esp_log.h"

#include "api/radio_api.h"
#include "config/pins.h"
#include "protocol/protocol_manager.h"
#if CONFIG_RADIO_ENABLE_CONSOLE
#include "console/console.h"
#endif

namespace {

constexpr const char* kTag = "main";

// Static so the driver's DMA buffers live in internal RAM and outlive app_main.
protocol::ProcessManager g_manager;
api::RadioApi g_api(g_manager);

}  // namespace

extern "C" void app_main() {
    const cc1120::Pins radioPins{
        .host = pins::kSpiHost,
        .sclk = pins::kSclk,
        .miso = pins::kMiso,
        .mosi = pins::kMosi,
        .csn = pins::kCsn,
        .resetN = pins::kResetN,
        .clockHz = pins::kSpiClockHz,
    };

    // Brings the radio up in RX with the RF switch on the RX path. On failure the
    // manager's task keeps retrying, so an unattended unit recovers by itself.
    if (esp_err_t err = g_manager.begin(radioPins); err != ESP_OK) {
        ESP_LOGE(kTag, "radio start failed: %s", esp_err_to_name(err));
    }

#if CONFIG_RADIO_ENABLE_CONSOLE
    ESP_ERROR_CHECK(console::start(g_manager.radio(), g_api));
#endif
    // app_main returns; the manager task (and console REPL, if enabled) keep running.
    // Application code on this unit calls g_api.send() / g_api.receive().
}
