#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

#include "api/radio_api.h"
#include "protocol/protocol_manager.h"

namespace stream {

// Data stream: a numbered sequence of binary packets, one radio packet each.
//
//   byte 0     kMagic (0xA5). Not printable ASCII, so a stream packet is never
//              mistaken for a command frame (command/message.h) and vice versa.
//   byte 1     flags (kFlagPattern, kFlagLast)
//   byte 2     stream id, 1-255, chosen by the sender. A new id restarts the
//              receiver's statistics.
//   bytes 3-4  sequence number, little-endian, 0-based
//   bytes 5-6  total packets in the stream, little-endian (0 = open-ended)
//   byte 7     header check: low byte of CRC-16 over bytes 0-6
//   bytes 8..  data, 0..kMaxData bytes
//
// The header check lets a receiver attribute a packet that failed the radio's
// CRC to its sequence number. With kFlagPattern the data is a pseudo-random
// pattern derived from (stream id, seq), so the receiver can regenerate it and
// count individual bit errors.

constexpr uint8_t kMagic = 0xA5;
constexpr size_t kHeaderLen = 8;
constexpr size_t kMaxData = protocol::kMaxPayload - kHeaderLen;
constexpr uint16_t kMaxCount = 1024;  // per-sequence loss/duplicate tracking limit

constexpr uint8_t kFlagPattern = 0x01;
constexpr uint8_t kFlagLast = 0x02;

struct Header {
    uint8_t flags;
    uint8_t sid;
    uint16_t seq;
    uint16_t count;
};

// Builds a packet into out. Returns its length, or 0 if it does not fit.
size_t encode(const Header& h, const uint8_t* data, size_t len, uint8_t* out, size_t cap);

// Decodes and checks the header. False if pkt is not a stream packet or the
// header check fails.
bool decodeHeader(const uint8_t* pkt, size_t len, Header& out);

// Deterministic test data for packet (sid, seq).
void fillPattern(uint8_t sid, uint16_t seq, uint8_t* out, size_t len);

// Estimated on-air time of one packet with the default preset (1.2 kbps).
uint32_t airtimeMs(size_t payloadLen);

struct SendConfig {
    uint8_t sid;      // 1-255
    uint16_t count;   // 1..kMaxCount
    uint8_t dataLen;  // 0..kMaxData
    uint32_t gapMs;   // extra idle time between packets
};

// Called after each packet with its sequence number and transmit result.
// Return false to stop the stream early.
using SendHook = bool (*)(uint16_t seq, esp_err_t err, void* ctx);

// Transmits a test-pattern stream. Blocks for roughly
// count * (airtime + gapMs). sent/failed count transmit results. Returns
// ESP_ERR_INVALID_ARG for a bad config, otherwise ESP_OK unless every packet failed.
esp_err_t sendPattern(api::RadioApi& api, const SendConfig& cfg, uint16_t& sent,
                      uint16_t& failed, SendHook hook = nullptr, void* ctx = nullptr);

// Receive-side statistics for one stream.
struct Summary {
    uint8_t sid;              // 0 = no stream received yet
    uint16_t count;           // expected packets (0 = open-ended)
    uint16_t ok;              // sequence numbers received intact
    uint16_t corrupt;         // received, but only with errors
    uint16_t lost;            // never received
    uint16_t duplicates;
    uint16_t outOfOrder;
    uint16_t garbled;         // packets too damaged to identify
    uint16_t crcErrors;       // packets that failed the radio CRC
    uint16_t payloadErrors;   // passed the radio CRC but data did not match the pattern
    uint32_t packets;         // all stream packets received, including duplicates
    uint32_t bitErrors;
    uint32_t bitsCompared;
    uint32_t okBytes;         // data bytes in intact packets (for goodput)
    uint32_t durationMs;      // first to last packet
    bool rssiValid;
    int16_t rssiAvg, rssiMin, rssiMax;
};

enum class RxResult : uint8_t { NotStream, Ok, Corrupt, Duplicate, Garbled };

// Tracks one incoming stream. Not thread-safe.
class Receiver {
public:
    void reset();

    // Feeds one received packet. Returns NotStream for anything that does not
    // start with kMagic.
    RxResult onPacket(const uint8_t* pkt, size_t len, const protocol::RxInfo& info);

    // Counts a packet that failed CRC and could not be identified at all (it may
    // have been a stream packet whose first byte was hit).
    void noteGarbled();

    // True if a stream packet arrived within the last few seconds.
    bool recentlyActive() const;

    bool started() const { return started_; }
    uint8_t sid() const { return sid_; }
    bool complete() const { return started_ && count_ > 0 && uniqueAny_ == count_; }
    bool lastSeen() const { return lastSeen_; }

    Summary summary() const;

private:
    void start(const Header& h);

    bool started_ = false;
    bool lastSeen_ = false;
    uint8_t sid_ = 0;
    uint16_t count_ = 0;
    int32_t highest_ = -1;
    uint16_t uniqueAny_ = 0;
    uint16_t uniqueOk_ = 0;
    uint16_t dup_ = 0;
    uint16_t ooo_ = 0;
    uint16_t garbled_ = 0;
    uint16_t crcErrors_ = 0;
    uint16_t payloadErrors_ = 0;
    uint32_t packets_ = 0;
    uint32_t bitErrors_ = 0;
    uint32_t bits_ = 0;
    uint32_t okBytes_ = 0;
    int32_t rssiSum_ = 0;
    uint32_t rssiN_ = 0;
    int16_t rssiMin_ = 0;
    int16_t rssiMax_ = 0;
    int64_t firstUs_ = 0;
    int64_t lastUs_ = 0;
    uint8_t anyBits_[kMaxCount / 8] = {};
    uint8_t okBits_[kMaxCount / 8] = {};
};

// Writes a compact "sid=..;n=..;ok=.." body for an RSP::STREAM reply. Only the
// fields that fit in one frame are sent: sid n ok cor lost dup gbl be bits rssi.
// Returns the length, or 0 if it does not fit.
size_t formatSummary(char* out, size_t cap, const Summary& s);

// Parses a body written by formatSummary. Missing fields are left at zero.
// Returns false if there is no sid field.
bool parseSummary(const char* body, Summary& out);

}  // namespace stream
