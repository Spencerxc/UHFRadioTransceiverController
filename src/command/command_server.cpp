#include "command/command_server.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "command/message.h"
#include "command/ping.h"
#include "command/telemetry.h"
#include "protocol/protocol_manager.h"
#include "stream/stream.h"
#include "util/crc16.h"

namespace command {

namespace {

static_assert(kMaxFrame == protocol::kMaxPayload, "frame size must match the radio payload");

constexpr const char* kTag = "command";

// Gives the requester time to see the STREAM::START ack before data arrives.
constexpr uint32_t kStreamStartDelayMs = 100;

api::RadioApi* g_api = nullptr;
stream::Receiver g_streamRx;  // statistics for the stream the base is sending us

// A stream requested by STREAM::START, sent after the ack has gone out.
bool g_streamPending = false;
stream::SendConfig g_streamCfg{};

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

// Reads an unsigned decimal field. A missing key leaves `value` unchanged.
// Returns false if the key is present but not a number in [lo, hi].
bool readU32(const char* body, const char* key, uint32_t lo, uint32_t hi, uint32_t& value) {
    char text[16];
    if (!findField(body, key, text, sizeof(text))) {
        return true;
    }
    char* end = nullptr;
    const unsigned long v = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0' || v < lo || v > hi) {
        return false;
    }
    value = uint32_t(v);
    return true;
}

// PING. With a `d` field this is a stress ping (see command/ping.h): check the
// request, apply any injected fault, and echo the data.
size_t handlePing(const Message& req, const protocol::RxInfo& rxInfo, const char* id, char* out,
                  size_t cap) {
    char d[kMaxFrame + 1];
    if (!findField(req.body, "d", d, sizeof(d))) {
        return reply(out, cap, "RSP", "PONG", id, "");
    }
    char f[4] = "";
    findField(req.body, "f", f, sizeof(f));
    if (std::strcmp(f, "d") == 0) {
        ESP_LOGI(kTag, "ping %s: injected drop, not replying", id);
        return 0;
    }

    const size_t dLen = std::strlen(d);
    const char* ul = "ok";
    char ck[8];
    if (!rxInfo.crcOk) {
        ul = "crc";
    } else if (findField(req.body, "ck", ck, sizeof(ck)) &&
               std::strtoul(ck, nullptr, 16) != util::crc16(d, dLen)) {
        ul = "data";
    }
    if (std::strcmp(f, "c") == 0) {
        corruptText(d, dLen);
    }

    char rs[8] = "NA";
    if (rxInfo.rssiValid) {
        std::snprintf(rs, sizeof(rs), "%d", int(rxInfo.rssiDbm));
    }
    char fields[kMaxFrame + 1];
    const int n = std::snprintf(fields, sizeof(fields), "ul=%s;rs=%s;d=%s", ul, rs, d);
    const size_t len = (n > 0 && size_t(n) < sizeof(fields)) ? reply(out, cap, "RSP", "PONG", id, fields) : 0;
    return len != 0 ? len : reply(out, cap, "ERR", "SIZE", id, "");
}

// STREAM::START::n=<count>;len=<bytes>;gap=<ms>;sid=<id> (all optional).
size_t handleStreamStart(const Message& req, const char* id, char* out, size_t cap) {
    uint32_t n = 50, len = 32, gap = 20, sid = 1 + esp_random() % 255;
    if (!readU32(req.body, "n", 1, stream::kMaxCount, n) ||
        !readU32(req.body, "len", 0, stream::kMaxData, len) ||
        !readU32(req.body, "gap", 0, 10000, gap) || !readU32(req.body, "sid", 1, 255, sid)) {
        return reply(out, cap, "ERR", "ARG", id, "cmd=STREAM::START");
    }
    g_streamCfg = stream::SendConfig{
        .sid = uint8_t(sid), .count = uint16_t(n), .dataLen = uint8_t(len), .gapMs = gap};
    g_streamPending = true;
    char fields[64];
    std::snprintf(fields, sizeof(fields), "sid=%lu;n=%lu;len=%lu;gap=%lu", (unsigned long)sid,
                  (unsigned long)n, (unsigned long)len, (unsigned long)gap);
    return reply(out, cap, "RSP", "STREAM", id, fields);
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
        return handlePing(req, rxInfo, id, out, cap);
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
        std::snprintf(fields, sizeof(fields), "state=%s;ver=%.24s;up=%lu;freq=%lu;ovf=%lu",
                      stateName(s.state), esp_app_get_description()->version,
                      (unsigned long)(s.uptimeMs / 1000), (unsigned long)s.frequencyHz,
                      (unsigned long)s.rxDropped);
        return reply(out, cap, "RSP", "STATUS", id, fields);
    }

    if (std::strcmp(req.verb, "GET") == 0 && std::strcmp(req.topic, "STREAM") == 0) {
        if (stream::formatSummary(fields, sizeof(fields), g_streamRx.summary()) == 0) {
            fields[0] = '\0';
        }
        const size_t n = reply(out, cap, "RSP", "STREAM", id, fields);
        return n != 0 ? n : reply(out, cap, "ERR", "SIZE", id, "");
    }

    if (std::strcmp(req.verb, "STREAM") == 0 && std::strcmp(req.topic, "START") == 0) {
        return handleStreamStart(req, id, out, cap);
    }

    if (std::strcmp(req.verb, "STREAM") == 0 && std::strcmp(req.topic, "RESET") == 0) {
        g_streamRx.reset();
        return reply(out, cap, "RSP", "STREAM", id, "sid=0");
    }

    std::snprintf(fields, sizeof(fields), "cmd=%s%s%s", req.verb, req.topic[0] ? "::" : "",
                  req.topic);
    return reply(out, cap, "ERR", "UNKNOWN", id, fields);
}

void sendRequestedStream() {
    vTaskDelay(pdMS_TO_TICKS(kStreamStartDelayMs));
    const stream::SendConfig cfg = g_streamCfg;
    ESP_LOGI(kTag, "stream sid=%u: sending %u x %u B, gap %lu ms", unsigned(cfg.sid),
             unsigned(cfg.count), unsigned(cfg.dataLen), (unsigned long)cfg.gapMs);
    uint16_t sent = 0;
    uint16_t failed = 0;
    const esp_err_t err = stream::sendPattern(*g_api, cfg, sent, failed);
    ESP_LOGI(kTag, "stream sid=%u done: %u sent, %u failed (%s)", unsigned(cfg.sid),
             unsigned(sent), unsigned(failed), esp_err_to_name(err));
    // Requests that arrived while we were transmitting are stale by now.
    uint8_t buf[protocol::kMaxPayload];
    size_t len = 0;
    while (g_api->receive(buf, sizeof(buf), len, 0) != ESP_ERR_TIMEOUT) {
    }
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

        // Stream data packets are counted, never answered.
        if (g_streamRx.onPacket(rx, len, info) != stream::RxResult::NotStream) {
            continue;
        }

        Message req;
        size_t outLen = 0;
        if (parse(rx, len, req)) {
            ESP_LOGI(kTag, "rx %.*s (rssi %d dBm, lqi %u, crc %s)", int(len),
                     reinterpret_cast<const char*>(rx), int(info.rssiDbm), unsigned(info.lqi),
                     info.crcOk ? "ok" : "BAD");
            outLen = handle(req, info, tx, sizeof(tx));
        } else if (!info.crcOk && g_streamRx.recentlyActive()) {
            // Probably a stream packet damaged beyond recognition. Replying would
            // collide with the rest of the stream, so just count it.
            g_streamRx.noteGarbled();
            continue;
        } else {
            ESP_LOGW(kTag, "rx unparseable frame (%u bytes, crc %s)", unsigned(len),
                     info.crcOk ? "ok" : "BAD");
            outLen = format(tx, sizeof(tx), "ERR", "PARSE", "%s", "");
        }
        if (outLen == 0) {
            continue;
        }

        if (esp_err_t sendErr = g_api->send(reinterpret_cast<const uint8_t*>(tx), outLen);
            sendErr != ESP_OK) {
            ESP_LOGE(kTag, "reply failed: %s", esp_err_to_name(sendErr));
            g_streamPending = false;
        } else {
            ESP_LOGI(kTag, "tx %.*s", int(outLen), tx);
        }

        if (g_streamPending) {
            g_streamPending = false;
            sendRequestedStream();
        }
    }
}

}  // namespace

esp_err_t start(api::RadioApi& api) {
    g_api = &api;
    if (xTaskCreate(run, "command", 6144, nullptr, 4, nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(kTag, "command server listening");
    return ESP_OK;
}

}  // namespace command
