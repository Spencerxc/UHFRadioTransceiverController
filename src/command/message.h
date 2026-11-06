#pragma once

#include <cstddef>
#include <cstdint>

namespace command {

// Over-the-air command & control frame (ASCII, one radio packet per message):
//
//   <VERB>::<TOPIC>::<BODY>
//
// BODY is a list of key=value pairs separated by ';', and may be empty. TOPIC and
// BODY may be omitted ("PING" and "PING::" are valid). An optional "id=<n>" field
// in a request is echoed in the reply so the sender can match the two.
//
//   GET::TEL::id=7     ->  RSP::TEL::id=7;up=125;tmp=23.4;bat=3.71;rx=12;tx=11;freq=434000000;rssi=-61
//   PING::::id=8       ->  RSP::PONG::id=8
//   GET::STATUS::id=9  ->  RSP::STATUS::id=9;state=RX;ver=...;up=125;freq=434000000
//   FOO::BAR::id=10    ->  ERR::UNKNOWN::id=10;cmd=FOO::BAR

// Largest frame; equal to protocol::kMaxPayload (checked in command_server.cpp).
constexpr size_t kMaxFrame = 125;

struct Message {
    char verb[8];     // upper-cased
    char topic[12];   // upper-cased, may be empty
    char body[kMaxFrame + 1];
};

// Parses a received frame. Verb and topic are upper-cased so matching is
// case-insensitive. Returns false for empty or over-long fields and for frames
// containing non-printable bytes.
bool parse(const uint8_t* data, size_t len, Message& out);

// Copies the value of `key` from a "k=v;k=v" body into out. Returns false if the
// key is missing or the value does not fit.
bool findField(const char* body, const char* key, char* out, size_t outLen);

// Writes "<verb>::<topic>::<body>" into out, where body is printf-formatted.
// Returns the frame length, or 0 if it would exceed kMaxFrame or cap - 1.
size_t format(char* out, size_t cap, const char* verb, const char* topic, const char* bodyFmt, ...)
    __attribute__((format(printf, 5, 6)));

// Calls fn(key, value) for each key=value field in body, in order. Fields with
// no '=' are passed with an empty value.
template <typename Fn>
void forEachField(const char* body, Fn fn) {
    char key[24];
    char value[kMaxFrame + 1];
    const char* p = body;
    while (*p != '\0') {
        size_t k = 0;
        while (*p != '\0' && *p != '=' && *p != ';') {
            if (k + 1 < sizeof(key)) key[k++] = *p;
            ++p;
        }
        key[k] = '\0';
        size_t v = 0;
        if (*p == '=') {
            ++p;
            while (*p != '\0' && *p != ';') {
                if (v + 1 < sizeof(value)) value[v++] = *p;
                ++p;
            }
        }
        value[v] = '\0';
        if (k > 0) fn(key, value);
        if (*p == ';') ++p;
    }
}

}  // namespace command
