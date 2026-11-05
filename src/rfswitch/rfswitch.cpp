#include "rfswitch/rfswitch.h"

#include "driver/gpio.h"

#include "config/pins.h"

namespace rfswitch {

namespace {

// RF front-end truth table (two select lines, 3.3 V logic):
//
//   kRfSwitchTx  kRfSwitchRx  Mode
//   -----------  -----------  --------
//   High         Low          Transmit (hardware power-on default)
//   Low          High         Receive
//
// Both-high and both-low are not valid selections. Transitions de-assert the
// active line before asserting the other so both lines are never high at once.

constexpr uint32_t kHigh = 1;
constexpr uint32_t kLow = 0;

esp_err_t drive(gpio_num_t deassert, gpio_num_t assert) {
    if (esp_err_t err = gpio_set_level(deassert, kLow); err != ESP_OK) {
        return err;
    }
    return gpio_set_level(assert, kHigh);
}

}  // namespace

esp_err_t init() {
    // Preload the output latches with the Rx levels before enabling the drivers
    // so neither pin glitches into the Tx state when it becomes an output.
    if (esp_err_t err = gpio_set_level(pins::kRfSwitchTx, kLow); err != ESP_OK) {
        return err;
    }
    if (esp_err_t err = gpio_set_level(pins::kRfSwitchRx, kHigh); err != ESP_OK) {
        return err;
    }
    gpio_config_t out{};
    out.pin_bit_mask = (1ULL << pins::kRfSwitchRx) | (1ULL << pins::kRfSwitchTx);
    out.mode = GPIO_MODE_INPUT_OUTPUT;  // input path lets isTx()/isRx() read the pins back
    if (esp_err_t err = gpio_config(&out); err != ESP_OK) {
        return err;
    }
    return setRx();
}

esp_err_t setRx() { return drive(pins::kRfSwitchTx, pins::kRfSwitchRx); }
esp_err_t setTx() { return drive(pins::kRfSwitchRx, pins::kRfSwitchTx); }

bool isTx() {
    return gpio_get_level(pins::kRfSwitchTx) == int(kHigh) &&
           gpio_get_level(pins::kRfSwitchRx) == int(kLow);
}

bool isRx() {
    return gpio_get_level(pins::kRfSwitchRx) == int(kHigh) &&
           gpio_get_level(pins::kRfSwitchTx) == int(kLow);
}

}  // namespace rfswitch
