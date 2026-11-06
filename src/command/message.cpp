#include "command/message.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace command {

namespace {

constexpr const char* kSep = "::";
constexpr size_t kSepLen = 2;

// Copies [begin, end) into out (upper-cased if requested). False if it does not fit.
bool copyField(const char* begin, const char* end, char* out, size_t cap, bool upper) {
    const size_t n = size_t(end - begin);
    if (n + 1 > cap) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        out[i] = upper ? char(std::toupper(static_cast<unsigned char>(begin[i]))) : begin[i];
    }
    out[n] = '\0';
    return true;
}

}  // namespace

bool parse(const uint8_t* data, size_t len, Message& out) {
    if (data == nullptr || len == 0 || len > kMaxFrame) {
        return false;
    }
    char frame[kMaxFrame + 1];
    for (size_t i = 0; i < len; ++i) {
        if (!std::isprint(data[i])) {
            return false;
        }
        frame[i] = char(data[i]);
    }
    frame[len] = '\0';

    out = Message{};
    const char* end = frame + len;

    const char* verbEnd = std::strstr(frame, kSep);
    if (verbEnd == nullptr) verbEnd = end;
    if (verbEnd == frame || !copyField(frame, verbEnd, out.verb, sizeof(out.verb), true)) {
        return false;
    }
    if (verbEnd == end) {
        return true;  // "PING"
    }

    const char* topic = verbEnd + kSepLen;
    const char* topicEnd = std::strstr(topic, kSep);
    if (topicEnd == nullptr) topicEnd = end;
    if (!copyField(topic, topicEnd, out.topic, sizeof(out.topic), true)) {
        return false;
    }
    if (topicEnd == end) {
        return true;  // "PING::" or "GET::TEL"
    }

    const char* body = topicEnd + kSepLen;
    return copyField(body, end, out.body, sizeof(out.body), false);
}

bool findField(const char* body, const char* key, char* out, size_t outLen) {
    bool found = false;
    forEachField(body, [&](const char* k, const char* v) {
        if (!found && std::strcmp(k, key) == 0 && std::strlen(v) < outLen) {
            std::strcpy(out, v);
            found = true;
        }
    });
    return found;
}

size_t format(char* out, size_t cap, const char* verb, const char* topic, const char* bodyFmt, ...) {
    const int head = std::snprintf(out, cap, "%s::%s::", verb, topic);
    if (head < 0 || size_t(head) >= cap) {
        return 0;
    }
    va_list args;
    va_start(args, bodyFmt);
    const int body = std::vsnprintf(out + head, cap - size_t(head), bodyFmt, args);
    va_end(args);
    if (body < 0) {
        return 0;
    }
    const size_t total = size_t(head) + size_t(body);
    if (total >= cap || total > kMaxFrame) {
        return 0;
    }
    return total;
}

}  // namespace command
