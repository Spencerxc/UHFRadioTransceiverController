#pragma once

#include <cstdint>

#include "esp_err.h"

#include "api/radio_api.h"
#include "command/message.h"

namespace command {

// Sends one request frame (e.g. "GET::TEL::" or "PING") and waits for the reply.
//
// An "id=<n>" field is appended to the request automatically and the first reply
// carrying the same id is returned; stale or unrelated packets are discarded.
// rttMs is measured from just before transmit until the reply is dequeued, so it
// includes the airtime of both packets. replyInfo gets the RSSI (dBm), LQI and
// CRC status of the reply as received here.
//
// Returns ESP_ERR_TIMEOUT if no matching reply arrives within timeoutMs, and
// ESP_ERR_INVALID_ARG if the request is not a valid frame or too long once the id
// is added.
esp_err_t request(api::RadioApi& api, const char* req, Message& reply, uint32_t timeoutMs,
                  uint32_t& rttMs, protocol::RxInfo& replyInfo);

}  // namespace command
