#pragma once

#include "esp_err.h"

#include "api/radio_api.h"

namespace command {

// Over-the-air command server for the remote unit (see command/message.h for
// the frame format). Starts a FreeRTOS task that waits on api.receive(), decodes
// each request, and transmits the reply:
//
//   PING          -> RSP::PONG    (stress ping with echo / fault injection, see command/ping.h)
//   GET::TEL      -> RSP::TEL     (uptime, rx/tx counts, frequency, RSSI of the request
//                                  in dBm, + simulated sensors)
//   GET::STATUS   -> RSP::STATUS  (radio state, firmware version, uptime, frequency,
//                                  RX queue overflows)
//   GET::STREAM   -> RSP::STREAM  (statistics of the last data stream received)
//   STREAM::START -> RSP::STREAM, then transmits a test-pattern data stream
//   STREAM::RESET -> RSP::STREAM  (clears the received-stream statistics)
//   anything else -> ERR::UNKNOWN (unparseable frames -> ERR::PARSE)
//
// Data stream packets (stream/stream.h) are counted and never answered.
// RSP/ERR frames are never answered, so two units cannot reply to each other forever.
// The task consumes every received packet, so it must not run alongside another
// receiver (the console's recv/cmd). Enabled by CONFIG_RADIO_ENABLE_COMMAND_SERVER.
// `api` must outlive the server.
esp_err_t start(api::RadioApi& api);

}  // namespace command
