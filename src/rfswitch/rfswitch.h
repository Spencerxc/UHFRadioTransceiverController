#pragma once

#include "esp_err.h"

namespace rfswitch {

// Drives the external RF switch select line (pins::kRfSwitch, 3.3 V logic).
// Low selects the RX path, High selects the TX path.

// Configures the pin as an output and drives it to the RX level.
esp_err_t init();

void setRx();
void setTx();
bool isTx();

}  // namespace rfswitch
