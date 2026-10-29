#include "rfswitch/rfswitch.h"

#include "driver/gpio.h"

#include "config/pins.h"

namespace rfswitch {

namespace {

constexpr uint32_t kLevelRx = 1;
constexpr uint32_t kLevelTx = 0;

}  // namespace

esp_err_t init() {
    // Set the output latch before enabling the driver so the pin never glitches high.
    if (esp_err_t err = gpio_set_level(pins::kRfSwitch, kLevelRx); err != ESP_OK) {
        return err;
    }
    gpio_config_t out{};
    out.pin_bit_mask = 1ULL << pins::kRfSwitch;
    out.mode = GPIO_MODE_INPUT_OUTPUT;  // input path lets isTx() read the pin back
    if (esp_err_t err = gpio_config(&out); err != ESP_OK) {
        return err;
    }
    return gpio_set_level(pins::kRfSwitch, kLevelRx);
}

void setRx() { gpio_set_level(pins::kRfSwitch, kLevelRx); }
void setTx() { gpio_set_level(pins::kRfSwitch, kLevelTx); }
bool isTx() { return gpio_get_level(pins::kRfSwitch) == int(kLevelTx); }

}  // namespace rfswitch
