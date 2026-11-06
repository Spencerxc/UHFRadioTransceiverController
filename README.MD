# Capstone: ESP32 ↔ CC1120 firmware

Microcontroller code for controlling a TI CC1120 dev board (420–470 MHz).
It runs on an ELEGOO ESP32 dev board (ESP32-WROOM-32) using ESP-IDF, which includes FreeRTOS. The code is written in C++.

The radio sits in **receive mode by default** and returns to RX after every transmit, so a unit with no console (the remote unit) boots straight into listening. For bench work, an optional serial command console over USB lets you read and write registers, send command strobes, check chip status, set the frequency, and send/receive packets.

## Software architecture

![Architecture stack](architecture_stack_no_uart_swd.svg)

| Layer | ID | Code |
|---|---|---|
| Interface API | 1U1SMGR2 | [src/api/](src/api/) `api::RadioApi`: `send`, `receive`, `setFrequency`, `status` |
| Protocol Process Manager | 1U1SMGR1 | [src/protocol/](src/protocol/) `protocol::ProcessManager`: state machine, packet framing, RX task, RF switch control |
| SPI Device Driver | 1U1SDD1 | [src/cc1120/](src/cc1120/) `cc1120::Cc1120` |
| RF Switch GPIO Driver | | [src/rfswitch/](src/rfswitch/) |

Each layer only calls the one below it. The debug console is a client of the Interface API (plus raw driver access for register debugging, which can disturb the Protocol Manager's state).

## Hardware wiring

The ESP32 column below is the GPIO number printed on the Elegoo board's
silkscreen. The CC1120 column uses the signal labels printed on the CC1120
board; GPIO1 is also the SPI `SO`/`CHIP_RDYn` signal.

| CC1120 board silk label | ESP32 board silk label | Notes |
|---|---|---|
| SCLK | 18 | VSPI clock |
| SO / GPIO1 | 19 | VSPI MISO; also CHIP_RDYn |
| SI | 23 | VSPI MOSI |
| CSn | 5 | Driven manually by the driver |
| RESET_N | 4 | |
| GPIO0 | 26 | Reserved for interrupts in Phase 2 |
| GPIO2 | 27 | Reserved for interrupts in Phase 2 |
| GPIO3 | 25 | Reserved |
| (RF switch Rx select) | 32 | 3.3 V logic to the external RF switch (see below) |
| (RF switch Tx select) | 33 | 3.3 V logic to the external RF switch (see below) |
| VDD | External | Driven by other power sources |
| GND | GND | |

RF switch truth table: **Tx High, Rx Low = Transmit** (hardware power-on state);
**Rx High, Tx Low = Receive** (selected by firmware at boot and after every transmit).

All pin assignments are in [src/config/pins.h](src/config/pins.h).

## Building and flashing

In order to connect to the ESP32 using the onboard USB-C to UART bridge, you will need to install the CP210x USB driver for your operating system. You can find the drivers here: [https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers).

You need VS Code with the PlatformIO extension. The first build downloads the ESP-IDF toolchain automatically. It can be found here: [https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide](https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide)

```sh
pio run                    # build
pio run -t upload          # flash (board connected over USB)
pio run -e remote          # remote-unit build: no serial console, command server on
pio run -e remote -t upload
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
| `send <text>` | Transmit a packet (max 125 bytes), then return to RX |
| `recv [ms]` | Wait for a received packet and print it as hex |
| `cmd <request> [ms]` | Send a command to the remote unit, wait for the reply (default 3000 ms) and print it with the round-trip time, e.g. `cmd PING`, `cmd GET::TEL::` |

Addresses `0x00`–`0x2E` are normal registers. Extended registers are written as `0x2Fxx`.

## Command & control protocol

The remote unit (`pio run -e remote`) runs a command server ([src/command/](src/command/)).
It listens in RX, decodes each request it receives, and transmits a reply. Messages are
ASCII, one radio packet each (max 125 bytes; the CC1120 appends 2 status bytes, RSSI and CRC/LQI, to each received packet, and these must fit in the 128-byte RX FIFO):

```
<VERB>::<TOPIC>::<BODY>        BODY = key=value pairs separated by ';' (may be empty)
```

Verb and topic are case-insensitive. If a request includes `id=<n>`, the reply copies it
back, which lets the sender match each reply to its request. The console's `cmd` command
adds the id automatically.

| Request | Reply | Fields |
|---|---|---|
| `PING` | `RSP::PONG::id=8` | (none). `cmd` prints the round-trip time |
| `GET::TEL::` | `RSP::TEL::id=7;up=125;tmp=23.4;bat=3.71;rx=12;tx=11;freq=434000000;rssi=-61` | `up` uptime s, `tmp` °C, `bat` V, `rx`/`tx` packet counts, `freq` Hz, `rssi` dBm of the request as received by the remote (`NA` if invalid) |
| `GET::STATUS::` | `RSP::STATUS::id=9;state=RX;ver=…;up=125;freq=434000000` | radio state, firmware version, uptime s, carrier Hz |
| anything else | `ERR::UNKNOWN::id=10;cmd=FOO::BAR` | |
| non-ASCII / malformed | `ERR::PARSE::` | |

> **`tmp` and `bat` are simulated** because no sensors are fitted yet. Temperature wanders randomly between 18 and 32 °C.
> Battery voltage drains slowly from 4.10 V with uptime. Uptime, packet counts and frequency are real.
> See [src/command/telemetry.cpp](src/command/telemetry.cpp).

> **RSSI** comes from the status byte the CC1120 appends to each received packet. The firmware
> converts it to dBm by subtracting the chip's typical offset of 102 dB (`protocol::kRssiOffsetDb`
> in [protocol_manager.h](src/protocol/protocol_manager.h)). Readings are approximate to within a
> few dB. For absolute accuracy, calibrate that constant against a known signal level.

The remote never answers `RSP`/`ERR` frames. The command server and the console can't be
enabled together, because both read received packets.

### Demo

Flash one board with `pio run -e remote -t upload` (the remote unit) and the other with
`pio run -t upload` (the base unit). Then, on the base unit's console:

```
cc1120> cmd PING
PONG  rtt = 312 ms  rssi = -58 dBm
cc1120> cmd GET::TEL::
RSP::TEL  rtt = 590 ms  rssi = -58 dBm (reply, measured here)
  up     = 125
  tmp    = 23.4
  bat    = 4.09
  rx     = 2
  tx     = 1
  freq   = 434000000
  rssi   = -61 dBm (request, measured by remote)
cc1120> cmd GET::STATUS::
cc1120> cmd FOO::BAR::        -> ERR::UNKNOWN
```

The values above are illustrative. At 1.2 kbps each byte takes about 7 ms on air, so the
round-trip time is mostly the airtime of the request plus the reply. If the remote is off or
out of range, `cmd` prints `no reply within 3000 ms`. A raw `send GET::TEL::` followed by
`recv 3000` also works; the reply then has no `id`.

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

If `PARTNUMBER` is `0xFF`, the CC1120 is not driving SO/MISO. Check that the radio is powered
at the voltage required by the board, shares ground with the ESP32, and that CSn, RESET_N,
and SO/MISO are wired to the pins above. `ESP_ERR_TIMEOUT` means SO stayed high while CSn
was asserted, which usually indicates the same wiring or power problem.

## Project layout

```
platformio.ini, CMakeLists.txt, sdkconfig.defaults   build configuration
src/main.cpp                    app_main: starts the Protocol Manager (and console or command server)
src/api/radio_api.h/.cpp        Interface API (1U1SMGR2)
src/command/message.*           command frame parse/format (VERB::TOPIC::k=v;...)
src/command/client.*            request/reply with id matching and RTT (used by console `cmd`)
src/command/command_server.*    remote-unit task answering PING / GET::TEL / GET::STATUS
src/command/telemetry.*         telemetry sample (real radio stats + simulated sensors)
src/protocol/protocol_manager.* Protocol Process Manager (1U1SMGR1)
src/rfswitch/rfswitch.h/.cpp    RF switch GPIO driver (Rx GPIO 32, Tx GPIO 33)
src/Kconfig.projbuild           RADIO_ENABLE_CONSOLE / RADIO_ENABLE_COMMAND_SERVER options
src/config/pins.h               pin map
src/cc1120/cc1120.h/.cpp        SPI driver (mutex-protected, FreeRTOS-safe)
src/cc1120/cc1120_regs.h        register map, strobes, status fields
src/cc1120/cc1120_presets.h     default 434 MHz register preset
src/console/console.h/.cpp      optional esp_console debug commands
Datasheets/cc1120.pdf           CC1120 datasheet
```

> The default preset is adapted from TI's EasyLink example settings. Regenerate it with **SmartRF Studio 7** for your exact board, crystal and data rate.
