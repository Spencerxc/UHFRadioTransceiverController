#include "command/ping.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_random.h"
#include "esp_timer.h"

#include "command/client.h"
#include "command/message.h"
#include "util/crc16.h"

namespace command {

namespace {

// Alphanumeric only, so the data never contains the ';' or '=' field separators.
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
constexpr size_t kAlphabetLen = sizeof(kAlphabet) - 1;

char randomChar() { return kAlphabet[esp_random() % kAlphabetLen]; }

esp_err_t finish(PingResult& r, PingOutcome o, const char* detail) {
    r.outcome = o;
    r.detail = detail;
    return ESP_OK;
}

}  // namespace

const char* outcomeName(PingOutcome o) {
    switch (o) {
        case PingOutcome::Ok: return "ok";
        case PingOutcome::Lost: return "LOST";
        case PingOutcome::CorruptUp: return "CORRUPT-UP";
        case PingOutcome::CorruptDown: return "CORRUPT-DOWN";
        default: return "ERROR";
    }
}

void corruptText(char* s, size_t len) {
    if (len == 0) {
        return;
    }
    const size_t i = esp_random() % len;
    char c;
    do {
        c = randomChar();
    } while (c == s[i]);
    s[i] = c;
}

esp_err_t ping(api::RadioApi& api, size_t dataLen, PingFault fault, uint32_t timeoutMs,
               PingResult& out) {
    out = PingResult{};
    out.detail = "";
    if (dataLen > kMaxPingData) {
        return ESP_ERR_INVALID_ARG;
    }

    char data[kMaxPingData + 1];
    for (size_t i = 0; i < dataLen; ++i) {
        data[i] = randomChar();
    }
    data[dataLen] = '\0';
    const uint16_t ck = util::crc16(data, dataLen);

    char wire[kMaxPingData + 1];
    std::memcpy(wire, data, dataLen + 1);
    if (fault == PingFault::CorruptUp) {
        corruptText(wire, dataLen);
    }
    const char* flag = fault == PingFault::Drop          ? ";f=d"
                       : fault == PingFault::CorruptDown ? ";f=c"
                                                         : "";

    const uint32_t id = nextRequestId();
    char frame[kMaxFrame + 1];
    const size_t frameLen = format(frame, sizeof(frame), "PING", "", "id=%lu;ck=%04X%s;d=%s",
                                   (unsigned long)id, unsigned(ck), flag, wire);
    if (frameLen == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t buf[protocol::kMaxPayload];
    size_t len = 0;
    while (api.receive(buf, sizeof(buf), len, 0) != ESP_ERR_TIMEOUT) {
    }

    const int64_t start = esp_timer_get_time();
    if (esp_err_t err = api.send(reinterpret_cast<const uint8_t*>(frame), frameLen);
        err != ESP_OK) {
        finish(out, PingOutcome::Error, "transmit failed");
        return err;
    }

    const int64_t deadline = start + int64_t(timeoutMs) * 1000;
    for (;;) {
        const int64_t now = esp_timer_get_time();
        if (now >= deadline) {
            return finish(out, PingOutcome::Lost, "no reply");
        }
        protocol::RxInfo info{};
        const esp_err_t err =
            api.receive(buf, sizeof(buf), len, uint32_t((deadline - now + 999) / 1000), &info);
        if (err == ESP_ERR_TIMEOUT) {
            return finish(out, PingOutcome::Lost, "no reply");
        }
        if (err != ESP_OK) {
            continue;
        }

        Message msg;
        if (!parse(buf, len, msg)) {
            if (!info.crcOk) {
                return finish(out, PingOutcome::CorruptDown, "unreadable reply (CRC fail)");
            }
            continue;  // not a command frame, e.g. a stream packet
        }
        if (std::strcmp(msg.verb, "ERR") == 0 && std::strcmp(msg.topic, "PARSE") == 0) {
            return finish(out, PingOutcome::CorruptUp, "remote could not parse request");
        }

        char got[16];
        const bool hasId = findField(msg.body, "id", got, sizeof(got));
        const unsigned long gotId = hasId ? std::strtoul(got, nullptr, 10) : 0;
        if (!hasId || gotId != id) {
            if (!info.crcOk) {
                return finish(out, PingOutcome::CorruptDown, "reply id garbled (CRC fail)");
            }
            if (hasId && gotId < id) {
                ++out.late;
            }
            continue;
        }

        out.replied = true;
        out.rttMs = uint32_t((esp_timer_get_time() - start) / 1000);
        out.reply = info;
        char rs[8];
        if (findField(msg.body, "rs", rs, sizeof(rs)) && std::strcmp(rs, "NA") != 0) {
            out.remoteRssiValid = true;
            out.remoteRssiDbm = int16_t(std::strtol(rs, nullptr, 10));
        }

        if (!info.crcOk) {
            return finish(out, PingOutcome::CorruptDown, "reply failed CRC");
        }
        if (std::strcmp(msg.verb, "RSP") != 0 || std::strcmp(msg.topic, "PONG") != 0) {
            return finish(out, PingOutcome::Error, "unexpected reply (not RSP::PONG)");
        }
        char ul[8] = "";
        findField(msg.body, "ul", ul, sizeof(ul));
        if (std::strcmp(ul, "crc") == 0) {
            return finish(out, PingOutcome::CorruptUp, "request failed CRC at remote");
        }
        if (std::strcmp(ul, "data") == 0) {
            return finish(out, PingOutcome::CorruptUp, "request data mismatch at remote");
        }
        char echo[kMaxFrame + 1];
        if (!findField(msg.body, "d", echo, sizeof(echo))) {
            return finish(out, PingOutcome::Error, "no echo in reply (remote firmware too old?)");
        }
        if (std::strcmp(echo, data) != 0) {
            return finish(out, PingOutcome::CorruptDown, "echo mismatch");
        }
        return finish(out, PingOutcome::Ok, "");
    }
}

}  // namespace command
