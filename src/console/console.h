#pragma once

#include "esp_err.h"

#include "api/radio_api.h"
#include "cc1120/cc1120.h"

namespace console {

// Registers the CC1120 commands and starts the esp_console REPL on UART0.
// The REPL runs in its own FreeRTOS task; this function returns immediately.
// Debug-only client of the Interface API; raw register commands go straight to
// the driver. Compiled in only when CONFIG_RADIO_ENABLE_CONSOLE is set.
// `radio` and `api` must outlive the console.
esp_err_t start(cc1120::Cc1120& radio, api::RadioApi& api);

}  // namespace console
