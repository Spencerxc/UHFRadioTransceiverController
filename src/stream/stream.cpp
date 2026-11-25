#include "stream/stream.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "command/message.h"
#include "util/crc16.h"

namespace stream {

namespace {

// Air-interface parameters of the default preset (cc1120_presets.h), used only
// to estimate airtime. Update them if the preset changes.
constexpr uint32_t kBitRateBps = 1200;
constexpr size_t kPreambleBytes = 4;  // PREAMBLE_CFG1 reset value
constexpr size_t kSyncBytes = 4;      // 32-bit sync word
constexpr size_t kLengthBytes = 1;    // variable-length mode
constexpr size_t kCrcBytes = 2;

constexpr int64_t kActiveUs = 5'000'000;

uint8_t headerCheck(const uint8_t* h) { return uint8_t(util::crc16(h, kHeaderLen - 1) & 0xFF); }

void put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

bool testBit(const uint8_t* bits, uint16_t i) { return (bits[i >> 3] & (1u << (i & 7))) != 0; }
void setBit(uint8_t* bits, uint16_t i) { bits[i >> 3] |= uint8_t(1u << (i & 7)); }

uint16_t sat16(uint32_t v, uint32_t max = 0xFFFF) { return uint16_t(std::min(v, max)); }

}  // namespace

size_t encode(const Header& h, const uint8_t* data, size_t len, uint8_t* out, size_t cap) {
    if (len > kMaxData || cap < kHeaderLen + len || (len > 0 && data == nullptr)) {
        return 0;
    }
    out[0] = kMagic;
    out[1] = h.flags;
    out[2] = h.sid;
    put16(out + 3, h.seq);
    put16(out + 5, h.count);
    out[7] = headerCheck(out);
    if (len > 0) {
        std::memcpy(out + kHeaderLen, data, len);
    }
    return kHeaderLen + len;
}

bool decodeHeader(const uint8_t* pkt, size_t len, Header& out) {
    if (pkt == nullptr || len < kHeaderLen || pkt[0] != kMagic || pkt[7] != headerCheck(pkt)) {
        return false;
    }
    out.flags = pkt[1];
    out.sid = pkt[2];
    out.seq = get16(pkt + 3);
    out.count = get16(pkt + 5);
    return true;
}

void fillPattern(uint8_t sid, uint16_t seq, uint8_t* out, size_t len) {
    // xorshift32 seeded from (sid, seq).
    uint32_t x = ((uint32_t(sid) << 16) | seq) * 2654435761u ^ 0x9E3779B9u;
    if (x == 0) x = 1;
    for (size_t i = 0; i < len; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        out[i] = uint8_t(x);
    }
}

uint32_t airtimeMs(size_t payloadLen) {
    const size_t bytes = kPreambleBytes + kSyncBytes + kLengthBytes + payloadLen + kCrcBytes;
    return uint32_t((bytes * 8 * 1000 + kBitRateBps - 1) / kBitRateBps);
}

esp_err_t sendPattern(api::RadioApi& api, const SendConfig& cfg, uint16_t& sent,
                      uint16_t& failed, SendHook hook, void* ctx) {
    sent = 0;
    failed = 0;
    if (cfg.sid == 0 || cfg.count == 0 || cfg.count > kMaxCount || cfg.dataLen > kMaxData) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t data[kMaxData];
    uint8_t pkt[protocol::kMaxPayload];
    esp_err_t last = ESP_OK;
    for (uint16_t seq = 0; seq < cfg.count; ++seq) {
        if (seq > 0 && cfg.gapMs > 0) {
            vTaskDelay(pdMS_TO_TICKS(cfg.gapMs));
        }
        const Header h{
            .flags = uint8_t(kFlagPattern | (seq + 1 == cfg.count ? kFlagLast : 0)),
            .sid = cfg.sid,
            .seq = seq,
            .count = cfg.count,
        };
        fillPattern(cfg.sid, seq, data, cfg.dataLen);
        const size_t n = encode(h, data, cfg.dataLen, pkt, sizeof(pkt));
        const esp_err_t err = api.send(pkt, n);
        if (err == ESP_OK) {
            ++sent;
        } else {
            ++failed;
            last = err;
        }
        if (hook != nullptr && !hook(seq, err, ctx)) {
            break;
        }
    }
    return sent > 0 ? ESP_OK : last;
}

// ---- Receiver ---------------------------------------------------------------

void Receiver::reset() { *this = Receiver{}; }

void Receiver::start(const Header& h) {
    reset();
    started_ = true;
    sid_ = h.sid;
    count_ = h.count;
}

bool Receiver::recentlyActive() const {
    return started_ && esp_timer_get_time() - lastUs_ < kActiveUs;
}

void Receiver::noteGarbled() {
    if (started_) {
        ++garbled_;
        lastUs_ = esp_timer_get_time();
    }
}

RxResult Receiver::onPacket(const uint8_t* pkt, size_t len, const protocol::RxInfo& info) {
    if (pkt == nullptr || len == 0 || pkt[0] != kMagic) {
        return RxResult::NotStream;
    }
    Header h{};
    if (!decodeHeader(pkt, len, h) || h.sid == 0 || h.count > kMaxCount ||
        (h.count > 0 && h.seq >= h.count) || h.seq >= kMaxCount) {
        noteGarbled();
        return RxResult::Garbled;
    }
    if (!started_ || h.sid != sid_) {
        start(h);
    }
    if (h.count != count_) {
        // Header passed its check but disagrees with the stream: treat as damaged.
        noteGarbled();
        return RxResult::Garbled;
    }

    const int64_t now = esp_timer_get_time();
    if (firstUs_ == 0) firstUs_ = now;
    lastUs_ = now;
    ++packets_;
    if (!info.crcOk) ++crcErrors_;
    if (info.rssiValid) {
        rssiSum_ += info.rssiDbm;
        rssiMin_ = rssiN_ == 0 ? info.rssiDbm : std::min(rssiMin_, info.rssiDbm);
        rssiMax_ = rssiN_ == 0 ? info.rssiDbm : std::max(rssiMax_, info.rssiDbm);
        ++rssiN_;
    }
    if (h.flags & kFlagLast) lastSeen_ = true;

    const size_t dataLen = len - kHeaderLen;
    uint32_t errs = 0;
    if (h.flags & kFlagPattern) {
        uint8_t expect[kMaxData];
        fillPattern(h.sid, h.seq, expect, dataLen);
        for (size_t i = 0; i < dataLen; ++i) {
            errs += uint32_t(__builtin_popcount(unsigned(expect[i] ^ pkt[kHeaderLen + i])));
        }
        bitErrors_ += errs;
        bits_ += uint32_t(dataLen * 8);
    }
    if (info.crcOk && errs > 0) ++payloadErrors_;
    const bool good = info.crcOk && errs == 0;

    const bool dup = testBit(anyBits_, h.seq);
    if (!dup) {
        setBit(anyBits_, h.seq);
        ++uniqueAny_;
        if (int32_t(h.seq) < highest_) ++ooo_;
    }
    highest_ = std::max(highest_, int32_t(h.seq));
    if (good && !testBit(okBits_, h.seq)) {
        setBit(okBits_, h.seq);
        ++uniqueOk_;
        okBytes_ += uint32_t(dataLen);
    }
    if (dup) {
        ++dup_;
        return RxResult::Duplicate;
    }
    return good ? RxResult::Ok : RxResult::Corrupt;
}

Summary Receiver::summary() const {
    Summary s{};
    if (!started_) {
        return s;
    }
    s.sid = sid_;
    s.count = count_;
    s.ok = uniqueOk_;
    s.corrupt = uint16_t(uniqueAny_ - uniqueOk_);
    const uint32_t expected = count_ > 0 ? count_ : uint32_t(highest_ + 1);
    s.lost = uint16_t(expected > uniqueAny_ ? expected - uniqueAny_ : 0);
    s.duplicates = dup_;
    s.outOfOrder = ooo_;
    s.garbled = garbled_;
    s.crcErrors = crcErrors_;
    s.payloadErrors = payloadErrors_;
    s.packets = packets_;
    s.bitErrors = bitErrors_;
    s.bitsCompared = bits_;
    s.okBytes = okBytes_;
    s.durationMs = uint32_t((lastUs_ - firstUs_) / 1000);
    s.rssiValid = rssiN_ > 0;
    if (s.rssiValid) {
        s.rssiAvg = int16_t(rssiSum_ / int32_t(rssiN_));
        s.rssiMin = rssiMin_;
        s.rssiMax = rssiMax_;
    }
    return s;
}

// ---- wire summary -----------------------------------------------------------

size_t formatSummary(char* out, size_t cap, const Summary& s) {
    char rssi[16] = "";
    if (s.rssiValid) {
        std::snprintf(rssi, sizeof(rssi), ";rssi=%d", int(s.rssiAvg));
    }
    const int n = std::snprintf(out, cap, "sid=%u;n=%u;ok=%u;cor=%u;lost=%u;dup=%u;gbl=%u;be=%lu;bits=%lu%s",
                                unsigned(s.sid), unsigned(s.count), unsigned(s.ok),
                                unsigned(s.corrupt), unsigned(s.lost),
                                unsigned(sat16(s.duplicates, 9999)), unsigned(sat16(s.garbled, 9999)),
                                (unsigned long)s.bitErrors, (unsigned long)s.bitsCompared, rssi);
    return (n < 0 || size_t(n) >= cap) ? 0 : size_t(n);
}

bool parseSummary(const char* body, Summary& out) {
    out = Summary{};
    bool hasSid = false;
    command::forEachField(body, [&](const char* k, const char* v) {
        const unsigned long u = std::strtoul(v, nullptr, 10);
        if (std::strcmp(k, "sid") == 0) {
            out.sid = uint8_t(u);
            hasSid = true;
        } else if (std::strcmp(k, "n") == 0) {
            out.count = sat16(u);
        } else if (std::strcmp(k, "ok") == 0) {
            out.ok = sat16(u);
        } else if (std::strcmp(k, "cor") == 0) {
            out.corrupt = sat16(u);
        } else if (std::strcmp(k, "lost") == 0) {
            out.lost = sat16(u);
        } else if (std::strcmp(k, "dup") == 0) {
            out.duplicates = sat16(u);
        } else if (std::strcmp(k, "gbl") == 0) {
            out.garbled = sat16(u);
        } else if (std::strcmp(k, "be") == 0) {
            out.bitErrors = uint32_t(u);
        } else if (std::strcmp(k, "bits") == 0) {
            out.bitsCompared = uint32_t(u);
        } else if (std::strcmp(k, "rssi") == 0) {
            out.rssiValid = true;
            out.rssiAvg = int16_t(std::strtol(v, nullptr, 10));
        }
    });
    return hasSid;
}

}  // namespace stream
