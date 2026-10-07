# Capstone: ESP32 ↔ CC1120 firmware

Microcontroller code for controlling a TI CC1120 dev board (420–470 MHz).
It runs on an ELEGOO ESP32 dev board (ESP32-WROOM-32) using ESP-IDF, which includes FreeRTOS. The code is written in C++.

Phase 1, the current phase, provides a serial command console over USB for talking to the CC1120: reading and writing registers, sending command strobes, checking chip status, and setting the frequency.

## Hardware wiring

| CC1120 pin | ESP32 GPIO | Notes |
|---|---|---|
| SCLK | 18 | VSPI clock |
| SO (GPIO1) | 19 | VSPI MISO; also CHIP_RDYn |
| SI | 23 | VSPI MOSI |
| CSn | 5 | Driven manually by the driver |
| RESET_N | 4 | |
| GPIO0 | 26 | Reserved for interrupts in Phase 2 |
| GPIO2 | 27 | Reserved for interrupts in Phase 2 |
| GPIO3 | 25 | Reserved |
| VDD | 3V3 | **3.3 V only** |
| GND | GND | |

All pin assignments are in [src/config/pins.h](src/config/pins.h).

## Building and flashing

You need VS Code with the PlatformIO extension. The first build downloads the ESP-IDF toolchain automatically.

```sh
pio run                    # build
pio run -t upload          # flash (board connected over USB)
pio device monitor         # serial console, 115200 baud
```

You can also use the PlatformIO toolbar buttons for Build, Upload and Monitor.

## Console commands

Type `help` at the `cc1120>` prompt to list all commands.

| Command | Description |
|---|---|
| `id` | Read PARTNUMBER (CC1120 = `0x48`) and PARTVERSION |
| `status` | Show the status byte (CHIP_RDYn and state), MARCSTATE, and RX/TX FIFO byte counts |
| `reset [hard\|soft]` | `hard` pulses RESET_N; `soft` sends the SRES strobe |
| `strobe <name\|hex>` | Send a command strobe: `SRES SFSTXON SXOFF SCAL SRX STX SIDLE SAFC SWOR SPWD SFRX SFTX SWORRST SNOP` |
| `rd <addr> [count]` | Read registers, e.g. `rd 0x0C`, `rd 0x2F8F`, or `rd 0x2F0C 3` |
| `wr <addr> <value>` | Write a register, e.g. `wr 0x04 0xAB` |
| `dump` | Dump all configuration registers (normal and extended) |
| `preset` | Load the default 434 MHz, 1.2 kbps 2-FSK configuration and calibrate |
| `freq [MHz]` | Get the carrier frequency, or set it (410–480 MHz) and recalibrate |

Addresses `0x00`–`0x2E` are normal registers. Extended registers are written as `0x2Fxx`.

### Quick hardware check

```
cc1120> id            -> PARTNUMBER 0x48 (CC1120)
cc1120> status        -> state=IDLE
cc1120> wr 0x04 0xAB
cc1120> rd 0x04       -> 0x0004: AB
cc1120> preset
cc1120> freq          -> 434.0000 MHz
cc1120> strobe SRX
cc1120> status        -> state=RX
cc1120> strobe SIDLE
```

If commands fail with `ESP_ERR_TIMEOUT`, the CC1120 never pulled SO low (CHIP_RDYn). Check the power, ground, CSn and MISO wiring.

## Project layout

```
platformio.ini, CMakeLists.txt, sdkconfig.defaults   build configuration
src/main.cpp                    app_main: starts the CC1120 driver and the console
src/config/pins.h               pin map
src/cc1120/cc1120.h/.cpp        SPI driver (mutex-protected, FreeRTOS-safe)
src/cc1120/cc1120_regs.h        register map, strobes, status fields
src/cc1120/cc1120_presets.h     default 434 MHz register preset
src/console/console.h/.cpp      esp_console command handlers
Datasheets/cc1120.pdf           CC1120 datasheet
```

> The default preset is adapted from TI's EasyLink example settings. Regenerate it with **SmartRF Studio 7** for your exact board, crystal and data rate.

## Roadmap

- **Phase 2: Packet TX/RX.** Send and receive variable-length packets through the 128-byte FIFOs. A CC1120 GPIO0/GPIO2 interrupt (`PKT_SYNC_RXTX`, `RXFIFO_THR`) feeds a FreeRTOS queue that an RX task drains. Adds `tx <data>` and `rx` console commands, plus RSSI and LQI readout for each packet. The driver already has `readRxFifo()` and `writeTxFifo()`.
- **Phase 3: Two-board link test.** Two ESP32 + CC1120 nodes ping-pong packets with sequence numbers. They report RSSI, LQI, packet loss and round-trip time, and are controlled from the console (`link master` / `link slave`).
