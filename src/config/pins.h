#pragma once

// ESP32 <-> CC1120 wiring. All hardware pin assignments live here.
//
//   CC1120         ESP32
//   CC1120 silk label       ESP32 silk label
//   SCLK                     18 (VSPI CLK)
//   SO/GPIO1                 19 (VSPI MISO)
//   SI                       23 (VSPI MOSI)
//   CSn                      5  (driven manually as a GPIO)
//   RESET_N                  4
//   GPIO0                    26
//   GPIO2                    27
//   GPIO3                    25
//   VDD/GND                  External
//
//   External RF switch select (3.3 V logic): GPIO 32
//     Low  = RX path (default / idle),  High = TX path

#include "driver/gpio.h"
#include "driver/spi_master.h"

namespace pins {

constexpr spi_host_device_t kSpiHost = SPI3_HOST;  // "VSPI" on the ESP32

constexpr gpio_num_t kSclk = GPIO_NUM_18;
constexpr gpio_num_t kMiso = GPIO_NUM_19;
constexpr gpio_num_t kMosi = GPIO_NUM_23;
constexpr gpio_num_t kCsn = GPIO_NUM_5;
constexpr gpio_num_t kResetN = GPIO_NUM_4;

constexpr gpio_num_t kCcGpio0 = GPIO_NUM_26;
constexpr gpio_num_t kCcGpio2 = GPIO_NUM_27;
constexpr gpio_num_t kCcGpio3 = GPIO_NUM_25;

// RF switch select output. The radio is bidirectional, so this line steers the
// antenna path: Low = RX (default), High = TX. 3.3 V logic, no strapping role.
constexpr gpio_num_t kRfSwitch = GPIO_NUM_32;

// SPI clock. The CC1120 supports up to 10 MHz for single access; 4 MHz
// leaves margin for jumper wires and extended-register burst access.
constexpr int kSpiClockHz = 4 * 1000 * 1000;

}  // namespace pins
