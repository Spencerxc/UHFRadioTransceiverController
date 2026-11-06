#include "command/client.h"

#include <cstdio>
#include <cstring>

#include "esp_timer.h"

#include "protocol/protocol_manager.h"

namespace command {

namespace {

uint32_t g_nextId = 1;

}  // namespace

esp_err_t request(api::RadioApi& api, const char* req, Message& reply, uint32_t timeoutMs,
                  uint32_t& rttMs, protocol::RxInfo& replyInfo) {
    Message msg;
    if (req == nullptr ||
        !parse(reinterpret_cast<const uint8_t*>(req), std::strlen(req), msg)) {
        return ESP_ERR_INVALID_ARG;
    }

    // Rebuild the frame with our id so the reply can be matched to this request.
    const uint32_t id = g_nextId++;
    char frame[kMaxFrame + 1];
    const size_t frameLen = format(frame, sizeof(frame), msg.verb, msg.topic, "%s%sid=%lu",
                                   msg.body, msg.body[0] ? ";" : "", (unsigned long)id);
    if (frameLen == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Drop anything already queued so an old packet is not mistaken for the reply.
    uint8_t buf[protocol::kMaxPayload];
    size_t rxLen = 0;
    while (api.receive(buf, sizeof(buf), rxLen, 0) != ESP_ERR_TIMEOUT) {
    }

    const int64_t start = esp_timer_get_time();
    if (esp_err_t err = api.send(reinterpret_cast<const uint8_t*>(frame), frameLen);
        err != ESP_OK) {
        return err;
    }

    char want[16];
    std::snprintf(want, sizeof(want), "%lu", (unsigned long)id);
    const int64_t deadline = start + int64_t(timeoutMs) * 1000;
    for (;;) {
        const int64_t now = esp_timer_get_time();
        if (now >= deadline) {
            return ESP_ERR_TIMEOUT;
        }
        const uint32_t waitMs = uint32_t((deadline - now + 999) / 1000);
        esp_err_t err = api.receive(buf, sizeof(buf), rxLen, waitMs, &replyInfo);
        if (err == ESP_ERR_TIMEOUT) {
            return ESP_ERR_TIMEOUT;
        }
        if (err != ESP_OK || !parse(buf, rxLen, reply)) {
            continue;
        }
        char got[16];
        if (findField(reply.body, "id", got, sizeof(got)) && std::strcmp(got, want) == 0) {
            rttMs = uint32_t((esp_timer_get_time() - start) / 1000);
            return ESP_OK;
        }
    }
}

}  // namespace command
