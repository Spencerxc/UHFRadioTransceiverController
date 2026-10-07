#include "esp_log.h"

#include "cc1120/cc1120.h"
#include "config/pins.h"
#include "console/console.h"

namespace {

constexpr const char* kTag = "main";

// Static so the driver's DMA buffers live in internal RAM and outlive app_main.
cc1120::Cc1120 g_radio;

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

    if (esp_err_t err = g_radio.init(radioPins); err != ESP_OK) {
        // Keep going: the console is still useful for debugging wiring.
        ESP_LOGE(kTag, "CC1120 init failed: %s", esp_err_to_name(err));
    }

    ESP_ERROR_CHECK(console::start(g_radio));
    // app_main returns; the console REPL keeps running in its own task.
}
