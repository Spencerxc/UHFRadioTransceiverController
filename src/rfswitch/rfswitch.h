#pragma once

#include "esp_err.h"

namespace rfswitch {

// Drives the external RF front-end switch via two select lines
// (pins::kRfSwitchTx, pins::kRfSwitchRx; 3.3 V logic):
//   Tx High, Rx Low = Transmit
//   Rx High, Tx Low = Receive
// The hardware powers up in Transmit; init() moves it to Receive.

// Configures both pins as outputs and selects the Receive path.
esp_err_t init();

// Select the Receive / Transmit path. The active line is de-asserted before the
// other is asserted, so both lines are never high simultaneously.
esp_err_t setRx();
esp_err_t setTx();

// True only when the pins are in the exact valid state for that mode; both
// return false if the lines are in an invalid (both-low / both-high) state.
bool isTx();
bool isRx();

}  // namespace rfswitch
