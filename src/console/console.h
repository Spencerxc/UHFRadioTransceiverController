#pragma once

#include "esp_err.h"

#include "cc1120/cc1120.h"

namespace console {

// Registers the CC1120 commands and starts the esp_console REPL on UART0.
// The REPL runs in its own FreeRTOS task; this function returns immediately.
// `radio` must outlive the console.
esp_err_t start(cc1120::Cc1120& radio);

}  // namespace console
