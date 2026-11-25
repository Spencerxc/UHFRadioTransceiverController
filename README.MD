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
| `ping [-c n] [-s bytes] [-i ms] [-W ms] [--drop %] [--corrupt-up %] [--corrupt-down %] [-q]` | Stress ping the remote: loss/corruption per direction, RTT/RSSI/LQI stats, optional fault injection (see [Stress testing](#stress-testing)) |
| `stream <tx\|rx> [-n count] [-s bytes] [-g gap_ms]` | Run a test data stream to (`tx`) or from (`rx`) the remote and report loss, corruption, duplicates and bit error rate |

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
| `PING::::ck=<crc16>;[f=d\|c;]d=<data>` | `RSP::PONG::id=8;ul=ok;rs=-61;d=<data>` | Stress ping (console `ping`). `ul` = how the request arrived: `ok`, `crc` (radio CRC failed) or `data` (data ≠ `ck`); `rs` = request RSSI at the remote; `d` echoed. `f=d` = don't reply, `f=c` = corrupt the echo (fault injection) |
| `GET::STATUS::` | `RSP::STATUS::id=9;state=RX;ver=…;up=125;freq=434000000;ovf=0` | `ovf` = received packets dropped because the RX queue was full |
| `STREAM::START::n=50;len=32;gap=20;sid=7` | `RSP::STREAM::id=11;sid=7;n=50;len=32;gap=20`, then the data stream | Remote transmits a test-pattern stream (all fields optional) |
| `GET::STREAM::` | `RSP::STREAM::id=12;sid=7;n=50;ok=49;cor=1;lost=0;dup=0;gbl=0;be=3;bits=12800;rssi=-60` | Statistics for the last stream the remote received |
| `STREAM::RESET::` | `RSP::STREAM::id=13;sid=0` | Clear those statistics |
| `GET::TEL::` | `RSP::TEL::id=7;up=125;tmp=23.4;bat=3.71;rx=12;tx=11;freq=434000000;rssi=-61` | `up` uptime s, `tmp` °C, `bat` V, `rx`/`tx` packet counts, `freq` Hz, `rssi` dBm of the request as received by the remote (`NA` if invalid) |
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

## Data streams

A data stream is a numbered sequence of binary packets ([src/stream/](src/stream/)), one radio packet each:

```
A5 | flags | sid | seq (u16 LE) | count (u16 LE) | hdr check | data (0-117 bytes)
```

The first byte, `0xA5`, is not printable ASCII, so stream packets and command frames can never be
confused. The header check (low byte of CRC-16 over the header) lets the receiver match a packet
that failed the radio CRC to its sequence number. Test streams fill the data with a pseudo-random
pattern derived from `(sid, seq)`, so the receiver can regenerate it and count bit errors.

`stream::Receiver` sorts every sequence number into **ok**, **corrupt** (received, but only with
errors), or **lost** (never received). It also counts duplicates, reordered packets, garbled packets
that could not be identified, CRC failures, bit errors, RSSI and goodput. The remote unit counts the
stream packets it receives and never replies to them.

```
cc1120> stream tx -n 100 -s 64 -g 0     base -> remote, then reads GET::STREAM from the remote
cc1120> stream rx -n 100 -s 64          remote -> base, counted here
.......................C.....................D....?........   (. ok  C corrupt  D duplicate  ? garbled  E tx error)
```

Streams are limited to 1024 packets. At 1.2 kbps a 64-byte packet takes about 0.5 s on air,
so a full-length stream takes several minutes, and neither end can be interrupted until it finishes.
The default `-g 20` gap gives the receiver time to re-arm RX between packets. `-g 0`
deliberately stresses that turnaround.

## Stress testing

`ping` sends each request with random data and a CRC-16 of that data. The remote checks the
request and echoes the data back, so every failure can be put down to one direction:

| Outcome | Meaning |
|---|---|
| `ok` | Reply arrived intact and the echo matches |
| `LOST` | No reply before the timeout (`-W`) |
| `CORRUPT-UP` | The request was damaged: radio CRC failed at the remote, data ≠ checksum, or the remote answered `ERR::PARSE` |
| `CORRUPT-DOWN` | The reply was damaged: CRC failed here, the reply was unreadable, or the echo does not match |

With two boards on a bench the link is usually perfect. To check that the detection works anyway,
inject faults: `--drop` asks the remote not to reply, `--corrupt-up` changes a character after the
checksum is computed, and `--corrupt-down` asks the remote to change a character of the echo.
Injected and natural failures are counted separately:

```
cc1120> ping -c 200 -s 64 --drop 5 --corrupt-up 5 --corrupt-down 5 -q
--- ping statistics ---
200 sent (31 with injected faults)
link (no injection): 168 ok, 1 lost, 0 corrupt-up, 0 corrupt-down, 0 error  -> 0.6% failed
injected faults detected:  drop 11/11  corrupt-up 9/9  corrupt-down 11/11
rtt min/avg/max = 905/921/948 ms
rssi here   min/avg/max = -61/-58/-55 dBm
rssi remote min/avg/max = -62/-59/-57 dBm
```

`cmd PING` and a plain `PING` without a `d` field still get the short `RSP::PONG::id=<n>` reply.
Bad-CRC packets are delivered to the application, not flushed by the chip (`FIFO_CFG.CRC_AUTOFLUSH = 0`),
which is how corruption shows up instead of looking like loss.

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
src/command/ping.*              stress ping: echo check, outcome classification, fault injection
src/command/command_server.*    remote-unit task answering PING / GET / STREAM requests
src/stream/stream.*             data stream packet format, test-pattern sender, receiver statistics
src/util/crc16.h                CRC-16/CCITT used by stream headers and the stress ping
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
