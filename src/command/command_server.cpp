#include "command/command_server.h"

#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "command/message.h"
#include "command/telemetry.h"
#include "protocol/protocol_manager.h"

namespace command {

namespace {

static_assert(kMaxFrame == protocol::kMaxPayload, "frame size must match the radio payload");

constexpr const char* kTag = "command";

api::RadioApi* g_api = nullptr;

const char* stateName(protocol::State s) {
    switch (s) {
        case protocol::State::Rx: return "RX";
        case protocol::State::Tx: return "TX";
        default: return "INIT";
    }
}

// Builds a reply frame whose body is "id=<id>;<fields>" (each part only if present).
size_t reply(char* out, size_t cap, const char* verb, const char* topic, const char* id,
             const char* fields) {
    const bool hasId = id[0] != '\0';
    const bool hasFields = fields[0] != '\0';
    return format(out, cap, verb, topic, "%s%s%s%s", hasId ? "id=" : "", id,
                  hasId && hasFields ? ";" : "", fields);
}

// Fills `out` with the reply to `req`. Returns its length, or 0 for no reply.
size_t handle(const Message& req, const protocol::RxInfo& rxInfo, char* out, size_t cap) {
    if (std::strcmp(req.verb, "RSP") == 0 || std::strcmp(req.verb, "ERR") == 0) {
        return 0;  // never answer a reply
    }

    char id[16] = "";
    findField(req.body, "id", id, sizeof(id));
    char fields[kMaxFrame + 1] = "";

    if (std::strcmp(req.verb, "PING") == 0) {
        return reply(out, cap, "RSP", "PONG", id, "");
    }

    if (std::strcmp(req.verb, "GET") == 0 && std::strcmp(req.topic, "TEL") == 0) {
        const Telemetry t = sampleTelemetry(*g_api, rxInfo);
        char rssi[8] = "NA";
        if (t.rssiValid) {
            std::snprintf(rssi, sizeof(rssi), "%d", int(t.rssiDbm));
        }
        std::snprintf(fields, sizeof(fields),
                      "up=%lu;tmp=%.1f;bat=%.2f;rx=%lu;tx=%lu;freq=%lu;rssi=%s",
                      (unsigned long)t.uptimeS, double(t.temperatureC), double(t.batteryV),
                      (unsigned long)t.rxPackets, (unsigned long)t.txPackets,
                      (unsigned long)t.frequencyHz, rssi);
        return reply(out, cap, "RSP", "TEL", id, fields);
    }

    if (std::strcmp(req.verb, "GET") == 0 && std::strcmp(req.topic, "STATUS") == 0) {
        const api::Status s = g_api->status();
        std::snprintf(fields, sizeof(fields), "state=%s;ver=%.24s;up=%lu;freq=%lu",
                      stateName(s.state), esp_app_get_description()->version,
                      (unsigned long)(s.uptimeMs / 1000), (unsigned long)s.frequencyHz);
        return reply(out, cap, "RSP", "STATUS", id, fields);
    }

    std::snprintf(fields, sizeof(fields), "cmd=%s%s%s", req.verb, req.topic[0] ? "::" : "",
                  req.topic);
    return reply(out, cap, "ERR", "UNKNOWN", id, fields);
}

void run(void*) {
    uint8_t rx[protocol::kMaxPayload];
    char tx[kMaxFrame + 1];
    for (;;) {
        size_t len = 0;
        protocol::RxInfo info{};
        const esp_err_t err = g_api->receive(rx, sizeof(rx), len, portMAX_DELAY, &info);
        if (err != ESP_OK) {
            continue;  // timeout (should not happen with portMAX_DELAY) or oversize packet
        }

        Message req;
        size_t outLen = 0;
        if (parse(rx, len, req)) {
            ESP_LOGI(kTag, "rx %.*s (rssi %d dBm, lqi %u, crc %s)", int(len),
                     reinterpret_cast<const char*>(rx), int(info.rssiDbm), unsigned(info.lqi),
                     info.crcOk ? "ok" : "BAD");
            outLen = handle(req, info, tx, sizeof(tx));
        } else {
            ESP_LOGW(kTag, "rx unparseable frame (%u bytes)", unsigned(len));
            outLen = format(tx, sizeof(tx), "ERR", "PARSE", "%s", "");
        }
        if (outLen == 0) {
            continue;
        }

        if (esp_err_t sendErr = g_api->send(reinterpret_cast<const uint8_t*>(tx), outLen);
            sendErr != ESP_OK) {
            ESP_LOGE(kTag, "reply failed: %s", esp_err_to_name(sendErr));
        } else {
            ESP_LOGI(kTag, "tx %.*s", int(outLen), tx);
        }
    }
}

}  // namespace

esp_err_t start(api::RadioApi& api) {
    g_api = &api;
    if (xTaskCreate(run, "command", 4096, nullptr, 4, nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(kTag, "command server listening");
    return ESP_OK;
}

}  // namespace command
