#include "console/console.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cc1120/cc1120_presets.h"
#include "command/client.h"
#include "command/message.h"
#include "command/ping.h"
#include "stream/stream.h"

namespace console {

namespace {

using cc1120::Cc1120;

Cc1120* g_radio = nullptr;
api::RadioApi* g_api = nullptr;

struct StrobeName {
    const char* name;
    uint8_t cmd;
};

constexpr StrobeName kStrobes[] = {
    {"SRES", cc1120::SRES},   {"SFSTXON", cc1120::SFSTXON}, {"SXOFF", cc1120::SXOFF},
    {"SCAL", cc1120::SCAL},   {"SRX", cc1120::SRX},         {"STX", cc1120::STX},
    {"SIDLE", cc1120::SIDLE}, {"SAFC", cc1120::SAFC},       {"SWOR", cc1120::SWOR},
    {"SPWD", cc1120::SPWD},   {"SFRX", cc1120::SFRX},       {"SFTX", cc1120::SFTX},
    {"SWORRST", cc1120::SWORRST}, {"SNOP", cc1120::SNOP},
};

// Parses decimal or 0x-prefixed hex.
bool parseU32(const char* s, uint32_t& out) {
    char* end = nullptr;
    errno = 0;
    unsigned long v = std::strtoul(s, &end, 0);
    if (end == s || *end != '\0' || errno != 0) {
        return false;
    }
    out = uint32_t(v);
    return true;
}

// Checks that [addr, addr+count) lies inside one register space.
bool validRange(uint32_t addr, uint32_t count) {
    if (count == 0 || count > Cc1120::kFifoSize) {
        return false;
    }
    if ((addr & 0xFF00) == 0x2F00) {
        return (addr & 0xFF) + count - 1 <= 0xFF;
    }
    return addr + count - 1 <= cc1120::kLastConfigReg;
}

const char* g_cmd = "";

int fail(esp_err_t err) {
    std::printf("error: '%s' failed: %s (0x%X)\n", g_cmd, esp_err_to_name(err), unsigned(err));
    if (err == ESP_ERR_TIMEOUT) {
        std::printf("  CC1120 never pulled MISO (GPIO19) low after CSn (GPIO5) went low, so no SPI\n"
                    "  clocks were sent. Check chip power/GND, RESET_N (GPIO4), CSn and MISO wiring.\n");
    } else if (err == ESP_ERR_INVALID_STATE) {
        std::printf("  driver not initialised (init() failed at boot, see the boot log).\n");
    }
    return 1;
}

// Marks the start of a command and, unless skipChip is set, confirms a CC1120
// answers on the bus. With no chip attached MISO floats low, so every SPI
// transfer "succeeds" and returns zeros; this catches that case.
bool begin(const char* name, bool skipChip = true) {
    g_cmd = name;
    if (skipChip) {
        return true;
    }
    uint8_t part = 0;
    if (esp_err_t err = g_radio->partNumber(part); err != ESP_OK) {
        fail(err);
        return false;
    }
    if (part != cc1120::kPartNumberCc1120) {
        std::printf("error: '%s' aborted: PARTNUMBER read 0x%02X, expected 0x%02X.\n"
                    "  No CC1120 is responding (0x00/0xFF means MISO is floating or stuck).\n"
                    "  Check power, wiring, and that MISO (GPIO19) is connected to the chip's SO.\n",
                    name, part, cc1120::kPartNumberCc1120);
        return false;
    }
    return true;
}

void printHexRows(uint16_t base, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (i % 8 == 0) {
            std::printf("%s0x%04X:", i ? "\n" : "", unsigned(base + i));
        }
        std::printf(" %02X", data[i]);
    }
    std::printf("\n");
}

// ---- id ---------------------------------------------------------------------
int cmdId(int, char**) {
    if (!begin("id", true)) {
        return 1;
    }
    uint8_t part = 0;
    uint8_t version = 0;
    if (esp_err_t err = g_radio->partNumber(part); err != ESP_OK) {
        return fail(err);
    }
    if (esp_err_t err = g_radio->partVersion(version); err != ESP_OK) {
        return fail(err);
    }
    std::printf("PARTNUMBER 0x%02X%s\nPARTVERSION 0x%02X\n", part,
                part == cc1120::kPartNumberCc1120 ? " (CC1120)" : " (MISMATCH, expected 0x48)",
                version);
    return part == cc1120::kPartNumberCc1120 ? 0 : 1;
}

// ---- status -----------------------------------------------------------------
int cmdStatus(int, char**) {
    if (!begin("status")) {
        return 1;
    }
    uint8_t status = 0;
    uint8_t marc = 0;
    uint8_t rxBytes = 0;
    uint8_t txBytes = 0;
    if (esp_err_t err = g_radio->status(status); err != ESP_OK) {
        return fail(err);
    }
    if (esp_err_t err = g_radio->marcState(marc); err != ESP_OK) {
        return fail(err);
    }
    if (esp_err_t err = g_radio->readReg(cc1120::NUM_RXBYTES, rxBytes); err != ESP_OK) {
        return fail(err);
    }
    if (esp_err_t err = g_radio->readReg(cc1120::NUM_TXBYTES, txBytes); err != ESP_OK) {
        return fail(err);
    }
    std::printf("status 0x%02X  CHIP_RDYn=%d  state=%s\n", status,
                (status & cc1120::kStatusChipRdyN) ? 1 : 0, Cc1120::chipStateName(status));
    std::printf("MARCSTATE 0x%02X (%s)\n", marc, Cc1120::marcStateName(marc));
    std::printf("RX FIFO %u bytes, TX FIFO %u bytes\n", rxBytes, txBytes);
    return 0;
}

// ---- reset [hard|soft] ------------------------------------------------------
struct {
    arg_str* mode;
    struct arg_end* end;
} g_resetArgs;

int cmdReset(int argc, char** argv) {
    if (arg_parse(argc, argv, reinterpret_cast<void**>(&g_resetArgs)) != 0) {
        arg_print_errors(stdout, g_resetArgs.end, argv[0]);
        return 1;
    }
    if (!begin("reset", true)) {
        return 1;
    }
    bool soft = false;
    if (g_resetArgs.mode->count > 0) {
        const char* mode = g_resetArgs.mode->sval[0];
        if (strcasecmp(mode, "soft") == 0) {
            soft = true;
        } else if (strcasecmp(mode, "hard") != 0) {
            std::printf("mode must be 'hard' or 'soft'\n");
            return 1;
        }
    }
    esp_err_t err = soft ? g_radio->softReset() : g_radio->hardReset();
    if (err != ESP_OK) {
        return fail(err);
    }
    std::printf("%s reset done\n", soft ? "soft" : "hard");
    return 0;
}

// ---- strobe <name|hex> ------------------------------------------------------
struct {
    arg_str* cmd;
    struct arg_end* end;
} g_strobeArgs;

int cmdStrobe(int argc, char** argv) {
    if (arg_parse(argc, argv, reinterpret_cast<void**>(&g_strobeArgs)) != 0) {
        arg_print_errors(stdout, g_strobeArgs.end, argv[0]);
        return 1;
    }
    if (!begin("strobe")) {
        return 1;
    }
    const char* arg = g_strobeArgs.cmd->sval[0];
    int cmd = -1;
    for (const auto& s : kStrobes) {
        if (strcasecmp(arg, s.name) == 0) {
            cmd = s.cmd;
            break;
        }
    }
    uint32_t value = 0;
    if (cmd < 0 && parseU32(arg, value)) {
        cmd = int(value);
    }
    if (cmd < cc1120::SRES || cmd > cc1120::SNOP) {
        std::printf("unknown strobe '%s'. Valid:", arg);
        for (const auto& s : kStrobes) {
            std::printf(" %s", s.name);
        }
        std::printf(" (or 0x30-0x3D)\n");
        return 1;
    }
    uint8_t status = 0;
    if (esp_err_t err = g_radio->strobe(uint8_t(cmd), &status); err != ESP_OK) {
        return fail(err);
    }
    // The status byte reflects the state *before* the strobe took effect.
    std::printf("status 0x%02X (%s)\n", status, Cc1120::chipStateName(status));
    return 0;
}

// ---- rd <addr> [count] ------------------------------------------------------
struct {
    arg_str* addr;
    arg_str* count;
    struct arg_end* end;
} g_rdArgs;

int cmdRead(int argc, char** argv) {
    if (arg_parse(argc, argv, reinterpret_cast<void**>(&g_rdArgs)) != 0) {
        arg_print_errors(stdout, g_rdArgs.end, argv[0]);
        return 1;
    }
    if (!begin("rd")) {
        return 1;
    }
    uint32_t addr = 0;
    uint32_t count = 1;
    if (!parseU32(g_rdArgs.addr->sval[0], addr) ||
        (g_rdArgs.count->count > 0 && !parseU32(g_rdArgs.count->sval[0], count))) {
        std::printf("invalid number\n");
        return 1;
    }
    if (!validRange(addr, count)) {
        std::printf("invalid range: use 0x00-0x2E or 0x2F00-0x2FFF, count 1-128\n");
        return 1;
    }
    uint8_t buf[Cc1120::kFifoSize];
    if (esp_err_t err = g_radio->readBurst(uint16_t(addr), buf, count); err != ESP_OK) {
        return fail(err);
    }
    printHexRows(uint16_t(addr), buf, count);
    return 0;
}

// ---- wr <addr> <value> ------------------------------------------------------
struct {
    arg_str* addr;
    arg_str* value;
    struct arg_end* end;
} g_wrArgs;

int cmdWrite(int argc, char** argv) {
    if (arg_parse(argc, argv, reinterpret_cast<void**>(&g_wrArgs)) != 0) {
        arg_print_errors(stdout, g_wrArgs.end, argv[0]);
        return 1;
    }
    if (!begin("wr")) {
        return 1;
    }
    uint32_t addr = 0;
    uint32_t value = 0;
    if (!parseU32(g_wrArgs.addr->sval[0], addr) || !parseU32(g_wrArgs.value->sval[0], value) ||
        value > 0xFF) {
        std::printf("invalid number (value must be 0x00-0xFF)\n");
        return 1;
    }
    if (!validRange(addr, 1)) {
        std::printf("invalid address: use 0x00-0x2E or 0x2F00-0x2FFF\n");
        return 1;
    }
    if (esp_err_t err = g_radio->writeReg(uint16_t(addr), uint8_t(value)); err != ESP_OK) {
        return fail(err);
    }
    std::printf("0x%04X <- 0x%02X\n", unsigned(addr), unsigned(value));
    return 0;
}

// ---- dump -------------------------------------------------------------------
int cmdDump(int, char**) {
    if (!begin("dump")) {
        return 1;
    }
    uint8_t buf[Cc1120::kFifoSize];

    constexpr size_t kNormalCount = cc1120::kLastConfigReg + 1;
    if (esp_err_t err = g_radio->readBurst(0x0000, buf, kNormalCount); err != ESP_OK) {
        return fail(err);
    }
    std::printf("-- registers 0x00-0x%02X --\n", cc1120::kLastConfigReg);
    printHexRows(0x0000, buf, kNormalCount);

    constexpr size_t kExtCount = (cc1120::kLastExtConfigReg & 0xFF) + 1;
    if (esp_err_t err = g_radio->readBurst(cc1120::IF_MIX_CFG, buf, kExtCount); err != ESP_OK) {
        return fail(err);
    }
    std::printf("-- extended registers 0x2F00-0x%04X --\n", cc1120::kLastExtConfigReg);
    printHexRows(cc1120::IF_MIX_CFG, buf, kExtCount);
    return 0;
}

// ---- preset -----------------------------------------------------------------
int cmdPreset(int, char**) {
    if (!begin("preset")) {
        return 1;
    }
    if (esp_err_t err = g_radio->strobe(cc1120::SIDLE); err != ESP_OK) {
        return fail(err);
    }
    if (esp_err_t err = g_radio->applyPreset(cc1120::kPreset434MHz, cc1120::kPreset434MHzCount);
        err != ESP_OK) {
        return fail(err);
    }
    if (esp_err_t err = g_radio->strobe(cc1120::SCAL); err != ESP_OK) {
        return fail(err);
    }
    std::printf("applied %u registers (434 MHz preset), calibrating\n",
                unsigned(cc1120::kPreset434MHzCount));
    return 0;
}

// ---- freq [MHz] -------------------------------------------------------------
struct {
    arg_dbl* mhz;
    struct arg_end* end;
} g_freqArgs;

int cmdFreq(int argc, char** argv) {
    if (arg_parse(argc, argv, reinterpret_cast<void**>(&g_freqArgs)) != 0) {
        arg_print_errors(stdout, g_freqArgs.end, argv[0]);
        return 1;
    }
    if (!begin("freq")) {
        return 1;
    }
    if (g_freqArgs.mhz->count > 0) {
        const double mhz = g_freqArgs.mhz->dval[0];
        const double hz = mhz * 1e6;
        if (hz < Cc1120::kBandMinHz || hz > Cc1120::kBandMaxHz) {
            std::printf("frequency must be 410-480 MHz for this board\n");
            return 1;
        }
        if (esp_err_t err = g_api->setFrequency(uint32_t(std::llround(hz))); err != ESP_OK) {
            return fail(err);
        }
    }
    uint32_t hz = 0;
    if (esp_err_t err = g_radio->getFrequency(hz); err != ESP_OK) {
        return fail(err);
    }
    std::printf("%.4f MHz\n", hz / 1e6);
    return 0;
}

// ---- send <text> ------------------------------------------------------------
struct {
    arg_str* text;
    struct arg_end* end;
} g_sendArgs;

int cmdSend(int argc, char** argv) {
    if (arg_parse(argc, argv, reinterpret_cast<void**>(&g_sendArgs)) != 0) {
        arg_print_errors(stdout, g_sendArgs.end, argv[0]);
        return 1;
    }
    g_cmd = "send";
    const char* text = g_sendArgs.text->sval[0];
    const size_t len = std::strlen(text);
    if (esp_err_t err = g_api->send(reinterpret_cast<const uint8_t*>(text), len); err != ESP_OK) {
        return fail(err);
    }
    std::printf("sent %u bytes, back in RX\n", unsigned(len));
    return 0;
}

// ---- recv [ms] --------------------------------------------------------------
struct {
    arg_int* ms;
    struct arg_end* end;
} g_recvArgs;

int cmdRecv(int argc, char** argv) {
    if (arg_parse(argc, argv, reinterpret_cast<void**>(&g_recvArgs)) != 0) {
        arg_print_errors(stdout, g_recvArgs.end, argv[0]);
        return 1;
    }
    g_cmd = "recv";
    const uint32_t ms = g_recvArgs.ms->count > 0 ? uint32_t(g_recvArgs.ms->ival[0]) : 1000;
    uint8_t buf[protocol::kMaxPayload];
    size_t len = 0;
    protocol::RxInfo info{};
    esp_err_t err = g_api->receive(buf, sizeof(buf), len, ms, &info);
    if (err == ESP_ERR_TIMEOUT) {
        std::printf("no packet\n");
        return 0;
    }
    if (err != ESP_OK) {
        return fail(err);
    }
    if (info.rssiValid) {
        std::printf("rssi %d dBm, lqi %u, crc %s\n", int(info.rssiDbm), unsigned(info.lqi),
                    info.crcOk ? "ok" : "BAD");
    } else {
        std::printf("rssi n/a, lqi %u, crc %s\n", unsigned(info.lqi), info.crcOk ? "ok" : "BAD");
    }
    std::printf("%u bytes:", unsigned(len));
    for (size_t i = 0; i < len; ++i) {
        std::printf(" %02X", buf[i]);
    }
    std::printf("\n");
    return 0;
}

// ---- cmd <request> [ms] -----------------------------------------------------
struct {
    arg_str* request;
    arg_int* ms;
    struct arg_end* end;
} g_cmdArgs;

int cmdCommand(int argc, char** argv) {
    if (arg_parse(argc, argv, reinterpret_cast<void**>(&g_cmdArgs)) != 0) {
        arg_print_errors(stdout, g_cmdArgs.end, argv[0]);
        return 1;
    }
    g_cmd = "cmd";
    const uint32_t ms = g_cmdArgs.ms->count > 0 ? uint32_t(g_cmdArgs.ms->ival[0]) : 3000;
    command::Message reply;
    uint32_t rttMs = 0;
    protocol::RxInfo info{};
    esp_err_t err = command::request(*g_api, g_cmdArgs.request->sval[0], reply, ms, rttMs, info);
    if (err == ESP_ERR_TIMEOUT) {
        std::printf("no reply within %lu ms\n", (unsigned long)ms);
        return 1;
    }
    if (err == ESP_ERR_INVALID_ARG) {
        std::printf("error: not a valid request frame (VERB[::TOPIC[::k=v;...]], max %u bytes)\n",
                    unsigned(command::kMaxFrame));
        return 1;
    }
    if (err != ESP_OK) {
        return fail(err);
    }

    // RSSI of the reply as measured by this unit.
    char localRssi[24] = "rssi = n/a";
    if (info.rssiValid) {
        std::snprintf(localRssi, sizeof(localRssi), "rssi = %d dBm", int(info.rssiDbm));
    }

    if (std::strcmp(reply.topic, "PONG") == 0) {
        std::printf("PONG  rtt = %lu ms  %s\n", (unsigned long)rttMs, localRssi);
        return 0;
    }
    std::printf("%s::%s  rtt = %lu ms  %s (reply, measured here)\n", reply.verb, reply.topic,
                (unsigned long)rttMs, localRssi);
    command::forEachField(reply.body, [](const char* key, const char* value) {
        if (std::strcmp(key, "id") == 0) {
            return;
        }
        if (std::strcmp(key, "rssi") == 0) {
            std::printf("  %-6s = %s dBm (request, measured by remote)\n", key, value);
        } else {
            std::printf("  %-6s = %s\n", key, value);
        }
    });
    return std::strcmp(reply.verb, "ERR") == 0 ? 1 : 0;
}

// ---- ping [options] ---------------------------------------------------------
struct {
    arg_int* count;
    arg_int* size;
    arg_int* interval;
    arg_int* timeout;
    arg_int* drop;
    arg_int* corruptUp;
    arg_int* corruptDown;
    arg_lit* quiet;
    struct arg_end* end;
} g_pingArgs;

int intArg(arg_int* a, int def) { return a->count > 0 ? a->ival[0] : def; }

struct MinMax {
    int32_t sum = 0;
    uint32_t n = 0;
    int32_t min = 0;
    int32_t max = 0;
    void add(int32_t v) {
        min = n == 0 ? v : std::min(min, v);
        max = n == 0 ? v : std::max(max, v);
        sum += v;
        ++n;
    }
    int32_t avg() const { return n ? sum / int32_t(n) : 0; }
};

int cmdPing(int argc, char** argv) {
    if (arg_parse(argc, argv, reinterpret_cast<void**>(&g_pingArgs)) != 0) {
        arg_print_errors(stdout, g_pingArgs.end, argv[0]);
        return 1;
    }
    g_cmd = "ping";
    const int count = intArg(g_pingArgs.count, 10);
    const int size = intArg(g_pingArgs.size, 16);
    const int interval = intArg(g_pingArgs.interval, 0);
    const int timeout = intArg(g_pingArgs.timeout, 3000);
    const int pDrop = intArg(g_pingArgs.drop, 0);
    const int pUp = intArg(g_pingArgs.corruptUp, 0);
    const int pDown = intArg(g_pingArgs.corruptDown, 0);
    const bool quiet = g_pingArgs.quiet->count > 0;

    if (count < 1 || count > 10000 || size < 0 || size > int(command::kMaxPingData) ||
        interval < 0 || timeout < 1 || pDrop < 0 || pUp < 0 || pDown < 0 ||
        pDrop + pUp + pDown > 100) {
        std::printf("invalid options: count 1-10000, size 0-%u, fault percentages must sum to <= 100\n",
                    unsigned(command::kMaxPingData));
        return 1;
    }
    if (size == 0 && (pUp > 0 || pDown > 0)) {
        std::printf("corruption injection needs --size >= 1\n");
        return 1;
    }

    constexpr int kOutcomes = 5;
    uint32_t natural[kOutcomes] = {};   // by outcome, for pings with no injected fault
    uint32_t injected[4] = {};          // by PingFault
    uint32_t detected[4] = {};          // injected faults classified as expected
    uint32_t late = 0;
    MinMax rtt, rssiHere, rssiRemote, lqi;

    std::printf("PING remote: %d x %d data bytes, timeout %d ms", count, size, timeout);
    if (pDrop || pUp || pDown) {
        std::printf(", injecting drop %d%% / corrupt-up %d%% / corrupt-down %d%%", pDrop, pUp, pDown);
    }
    std::printf("\n");

    for (int i = 1; i <= count; ++i) {
        if (i > 1 && interval > 0) {
            vTaskDelay(pdMS_TO_TICKS(interval));
        }
        const int roll = int(esp_random() % 100);
        const command::PingFault fault = roll < pDrop               ? command::PingFault::Drop
                                         : roll < pDrop + pUp         ? command::PingFault::CorruptUp
                                         : roll < pDrop + pUp + pDown ? command::PingFault::CorruptDown
                                                                      : command::PingFault::None;
        command::PingResult r;
        if (esp_err_t err = command::ping(*g_api, size_t(size), fault, uint32_t(timeout), r);
            err != ESP_OK && err != ESP_ERR_TIMEOUT && r.outcome != command::PingOutcome::Error) {
            return fail(err);
        }
        late += r.late;

        if (fault == command::PingFault::None) {
            ++natural[int(r.outcome)];
        } else {
            const command::PingOutcome expect =
                fault == command::PingFault::Drop        ? command::PingOutcome::Lost
                : fault == command::PingFault::CorruptUp ? command::PingOutcome::CorruptUp
                                                         : command::PingOutcome::CorruptDown;
            ++injected[int(fault)];
            if (r.outcome == expect) ++detected[int(fault)];
        }
        if (r.outcome == command::PingOutcome::Ok) {
            rtt.add(int32_t(r.rttMs));
        }
        if (r.replied && r.reply.rssiValid) rssiHere.add(r.reply.rssiDbm);
        if (r.replied) lqi.add(r.reply.lqi);
        if (r.remoteRssiValid) rssiRemote.add(r.remoteRssiDbm);

        if (!quiet) {
            static const char* const kFaultTag[] = {"", "  [injected drop]", "  [injected corrupt-up]",
                                                    "  [injected corrupt-down]"};
            std::printf("%4d  %-12s", i, command::outcomeName(r.outcome));
            if (r.replied) {
                std::printf(" rtt %4lu ms", (unsigned long)r.rttMs);
                if (r.reply.rssiValid) std::printf("  rssi %d", int(r.reply.rssiDbm));
                if (r.remoteRssiValid) std::printf("/%d", int(r.remoteRssiDbm));
                std::printf(" dBm  lqi %u", unsigned(r.reply.lqi));
            }
            if (r.detail[0] != '\0') std::printf("  (%s)", r.detail);
            std::printf("%s\n", kFaultTag[int(fault)]);
        } else if (i % 10 == 0) {
            std::printf("  %d/%d\n", i, count);
        }
    }

    using O = command::PingOutcome;
    const uint32_t nNatural = natural[int(O::Ok)] + natural[int(O::Lost)] +
                              natural[int(O::CorruptUp)] + natural[int(O::CorruptDown)] +
                              natural[int(O::Error)];
    std::printf("--- ping statistics ---\n");
    std::printf("%d sent", count);
    const uint32_t nInjected = injected[1] + injected[2] + injected[3];
    if (nInjected > 0) std::printf(" (%lu with injected faults)", (unsigned long)nInjected);
    std::printf("\n");
    if (nNatural > 0) {
        const uint32_t bad = nNatural - natural[int(O::Ok)];
        std::printf("link (no injection): %lu ok, %lu lost, %lu corrupt-up, %lu corrupt-down, "
                    "%lu error  -> %.1f%% failed\n",
                    (unsigned long)natural[int(O::Ok)], (unsigned long)natural[int(O::Lost)],
                    (unsigned long)natural[int(O::CorruptUp)],
                    (unsigned long)natural[int(O::CorruptDown)],
                    (unsigned long)natural[int(O::Error)], 100.0 * bad / nNatural);
    }
    if (nInjected > 0) {
        const char* const names[] = {"", "drop", "corrupt-up", "corrupt-down"};
        std::printf("injected faults detected:");
        for (int f = 1; f <= 3; ++f) {
            if (injected[f] > 0) {
                std::printf("  %s %lu/%lu", names[f], (unsigned long)detected[f],
                            (unsigned long)injected[f]);
            }
        }
        std::printf("\n  (a miss means a natural failure hit the same ping first)\n");
    }
    if (late > 0) std::printf("late replies discarded: %lu\n", (unsigned long)late);
    if (rtt.n > 0) {
        std::printf("rtt min/avg/max = %ld/%ld/%ld ms\n", long(rtt.min), long(rtt.avg()),
                    long(rtt.max));
    }
    if (rssiHere.n > 0) {
        std::printf("rssi here   min/avg/max = %ld/%ld/%ld dBm\n", long(rssiHere.min),
                    long(rssiHere.avg()), long(rssiHere.max));
    }
    if (rssiRemote.n > 0) {
        std::printf("rssi remote min/avg/max = %ld/%ld/%ld dBm\n", long(rssiRemote.min),
                    long(rssiRemote.avg()), long(rssiRemote.max));
    }
    if (lqi.n > 0) {
        std::printf("lqi here    min/avg/max = %ld/%ld/%ld (lower is better)\n", long(lqi.min),
                    long(lqi.avg()), long(lqi.max));
    }
    return natural[int(O::Ok)] == nNatural ? 0 : 1;
}

// ---- stream <tx|rx> [options] -----------------------------------------------
struct {
    arg_str* dir;
    arg_int* count;
    arg_int* size;
    arg_int* gap;
    struct arg_end* end;
} g_streamArgs;

stream::Receiver g_streamRx;  // static: too big for the console task's stack

void progressChar(char c, uint32_t n) {
    std::putchar(c);
    if (n % 64 == 0) std::putchar('\n');
    std::fflush(stdout);
}

bool txHook(uint16_t seq, esp_err_t err, void*) {
    progressChar(err == ESP_OK ? '.' : 'E', uint32_t(seq) + 1);
    return true;
}

void printSummary(const char* who, const stream::Summary& s, bool detailed) {
    const auto pct = [&](uint32_t v) { return s.count ? 100.0 * v / s.count : 0.0; };
    std::printf("%s: stream sid=%u, %u packets expected\n", who, unsigned(s.sid), unsigned(s.count));
    std::printf("  ok         %5u  (%.1f%%)\n", unsigned(s.ok), pct(s.ok));
    std::printf("  corrupt    %5u  (%.1f%%)  received, but only with errors\n", unsigned(s.corrupt),
                pct(s.corrupt));
    std::printf("  lost       %5u  (%.1f%%)  never received\n", unsigned(s.lost), pct(s.lost));
    std::printf("  duplicate  %5u\n", unsigned(s.duplicates));
    std::printf("  garbled    %5u  unidentifiable packets\n", unsigned(s.garbled));
    if (detailed) {
        std::printf("  crc fail   %5u  packets (crc ok but wrong data: %u)\n", unsigned(s.crcErrors),
                    unsigned(s.payloadErrors));
        std::printf("  reordered  %5u\n", unsigned(s.outOfOrder));
    }
    if (s.bitsCompared > 0) {
        std::printf("  bit errors %lu / %lu bits  (BER %.2e)\n", (unsigned long)s.bitErrors,
                    (unsigned long)s.bitsCompared, double(s.bitErrors) / s.bitsCompared);
    }
    if (s.rssiValid) {
        if (detailed) {
            std::printf("  rssi       avg %d dBm (min %d, max %d)\n", int(s.rssiAvg), int(s.rssiMin),
                        int(s.rssiMax));
        } else {
            std::printf("  rssi       avg %d dBm\n", int(s.rssiAvg));
        }
    }
    if (detailed && s.durationMs > 0) {
        std::printf("  goodput    %.0f bit/s over %.1f s\n", s.okBytes * 8000.0 / s.durationMs,
                    s.durationMs / 1000.0);
    }
}

// Base -> remote: send the stream, then ask the remote what it saw.
int streamTx(const stream::SendConfig& cfg) {
    std::printf("sending %u x %u B (sid=%u, gap %lu ms, ~%lu ms airtime each)\n",
                unsigned(cfg.count), unsigned(cfg.dataLen), unsigned(cfg.sid),
                (unsigned long)cfg.gapMs,
                (unsigned long)stream::airtimeMs(stream::kHeaderLen + cfg.dataLen));
    const int64_t start = esp_timer_get_time();
    uint16_t sent = 0;
    uint16_t failed = 0;
    const esp_err_t err = stream::sendPattern(*g_api, cfg, sent, failed, txHook);
    const double secs = (esp_timer_get_time() - start) / 1e6;
    std::printf("\nsent %u, transmit failures %u, %.1f s (%.0f bit/s offered)\n", unsigned(sent),
                unsigned(failed), secs, secs > 0 ? sent * cfg.dataLen * 8 / secs : 0.0);
    if (err != ESP_OK) {
        return fail(err);
    }

    command::Message reply;
    uint32_t rttMs = 0;
    protocol::RxInfo info{};
    esp_err_t qerr = ESP_ERR_TIMEOUT;
    for (int attempt = 0; attempt < 3 && qerr == ESP_ERR_TIMEOUT; ++attempt) {
        qerr = command::request(*g_api, "GET::STREAM::", reply, 3000, rttMs, info);
    }
    if (qerr == ESP_ERR_TIMEOUT) {
        std::printf("remote did not answer GET::STREAM (3 tries)\n");
        return 1;
    }
    if (qerr != ESP_OK) {
        return fail(qerr);
    }
    stream::Summary s;
    if (std::strcmp(reply.topic, "STREAM") != 0 || !stream::parseSummary(reply.body, s)) {
        std::printf("unexpected reply %s::%s (remote firmware too old?)\n", reply.verb, reply.topic);
        return 1;
    }
    if (s.sid != cfg.sid) {
        std::printf("remote received no packets from this stream (its last stream: sid=%u)\n",
                    unsigned(s.sid));
        return 1;
    }
    printSummary("remote", s, false);
    return s.ok == cfg.count ? 0 : 1;
}

// Remote -> base: ask the remote to stream and count what arrives.
int streamRx(const stream::SendConfig& cfg) {
    char req[command::kMaxFrame + 1];
    std::snprintf(req, sizeof(req), "STREAM::START::n=%u;len=%u;gap=%lu;sid=%u",
                  unsigned(cfg.count), unsigned(cfg.dataLen), (unsigned long)cfg.gapMs,
                  unsigned(cfg.sid));
    command::Message reply;
    uint32_t rttMs = 0;
    protocol::RxInfo info{};
    esp_err_t err = command::request(*g_api, req, reply, 3000, rttMs, info);
    if (err == ESP_ERR_TIMEOUT) {
        std::printf("remote did not acknowledge STREAM::START\n");
        return 1;
    }
    if (err != ESP_OK) {
        return fail(err);
    }
    if (std::strcmp(reply.verb, "RSP") != 0) {
        std::printf("remote refused: %s::%s::%s\n", reply.verb, reply.topic, reply.body);
        return 1;
    }
    std::printf("receiving %u x %u B (sid=%u, gap %lu ms)\n", unsigned(cfg.count),
                unsigned(cfg.dataLen), unsigned(cfg.sid), (unsigned long)cfg.gapMs);

    // Give up once nothing has arrived for a few packet times.
    const uint32_t idleMs =
        stream::airtimeMs(stream::kHeaderLen + cfg.dataLen) * 2 + cfg.gapMs + 1500;
    g_streamRx.reset();
    uint8_t buf[protocol::kMaxPayload];
    uint32_t shown = 0;
    for (;;) {
        size_t len = 0;
        err = g_api->receive(buf, sizeof(buf), len, idleMs, &info);
        if (err == ESP_ERR_TIMEOUT) {
            std::printf("\n(nothing for %lu ms, stopping)", (unsigned long)idleMs);
            break;
        }
        if (err != ESP_OK) {
            continue;
        }
        stream::RxResult r = g_streamRx.onPacket(buf, len, info);
        if (r == stream::RxResult::NotStream) {
            if (info.crcOk) continue;  // stray command frame
            g_streamRx.noteGarbled();
            r = stream::RxResult::Garbled;
        }
        const char c = r == stream::RxResult::Ok          ? '.'
                       : r == stream::RxResult::Corrupt   ? 'C'
                       : r == stream::RxResult::Duplicate ? 'D'
                                                          : '?';
        progressChar(c, ++shown);
        if (g_streamRx.complete() || g_streamRx.lastSeen()) {
            break;
        }
    }
    std::printf("\n");
    if (!g_streamRx.started()) {
        std::printf("no stream packets received\n");
        return 1;
    }
    const stream::Summary s = g_streamRx.summary();
    printSummary("here", s, true);
    return s.ok == cfg.count ? 0 : 1;
}

int cmdStream(int argc, char** argv) {
    if (arg_parse(argc, argv, reinterpret_cast<void**>(&g_streamArgs)) != 0) {
        arg_print_errors(stdout, g_streamArgs.end, argv[0]);
        return 1;
    }
    g_cmd = "stream";
    const char* dir = g_streamArgs.dir->sval[0];
    const bool tx = strcasecmp(dir, "tx") == 0;
    if (!tx && strcasecmp(dir, "rx") != 0) {
        std::printf("direction must be 'tx' (base -> remote) or 'rx' (remote -> base)\n");
        return 1;
    }
    const int count = intArg(g_streamArgs.count, 50);
    const int size = intArg(g_streamArgs.size, 32);
    const int gap = intArg(g_streamArgs.gap, 20);
    if (count < 1 || count > stream::kMaxCount || size < 0 || size > int(stream::kMaxData) ||
        gap < 0 || gap > 10000) {
        std::printf("invalid options: count 1-%u, size 0-%u, gap 0-10000 ms\n",
                    unsigned(stream::kMaxCount), unsigned(stream::kMaxData));
        return 1;
    }
    const stream::SendConfig cfg{
        .sid = uint8_t(1 + esp_random() % 255),
        .count = uint16_t(count),
        .dataLen = uint8_t(size),
        .gapMs = uint32_t(gap),
    };
    return tx ? streamTx(cfg) : streamRx(cfg);
}

esp_err_t registerCmd(const char* name, const char* help, esp_console_cmd_func_t func,
                      void* argtable = nullptr) {
    esp_console_cmd_t cmd{};
    cmd.command = name;
    cmd.help = help;
    cmd.func = func;
    cmd.argtable = argtable;
    return esp_console_cmd_register(&cmd);
}

esp_err_t registerCommands() {
    g_resetArgs.mode = arg_str0(nullptr, nullptr, "<hard|soft>", "reset type (default hard)");
    g_resetArgs.end = arg_end(2);

    g_strobeArgs.cmd = arg_str1(nullptr, nullptr, "<strobe>", "strobe name (e.g. SRX) or 0x30-0x3D");
    g_strobeArgs.end = arg_end(2);

    g_rdArgs.addr = arg_str1(nullptr, nullptr, "<addr>", "register, e.g. 0x0C or 0x2F8F");
    g_rdArgs.count = arg_str0(nullptr, nullptr, "<count>", "burst length (default 1)");
    g_rdArgs.end = arg_end(2);

    g_wrArgs.addr = arg_str1(nullptr, nullptr, "<addr>", "register, e.g. 0x04 or 0x2F0C");
    g_wrArgs.value = arg_str1(nullptr, nullptr, "<value>", "byte value, e.g. 0xAB");
    g_wrArgs.end = arg_end(2);

    g_freqArgs.mhz = arg_dbl0(nullptr, nullptr, "<MHz>", "new carrier, 410-480");
    g_freqArgs.end = arg_end(2);

    g_sendArgs.text = arg_str1(nullptr, nullptr, "<text>", "payload to transmit (quote if it has spaces)");
    g_sendArgs.end = arg_end(2);

    g_recvArgs.ms = arg_int0(nullptr, nullptr, "<ms>", "wait time (default 1000)");
    g_recvArgs.end = arg_end(2);

    g_cmdArgs.request = arg_str1(nullptr, nullptr, "<request>", "e.g. PING, GET::TEL::, GET::STATUS::");
    g_cmdArgs.ms = arg_int0(nullptr, nullptr, "<ms>", "reply timeout (default 3000)");
    g_cmdArgs.end = arg_end(2);

    g_pingArgs.count = arg_int0("c", "count", "<n>", "pings to send (default 10)");
    g_pingArgs.size = arg_int0("s", "size", "<bytes>", "echoed data per ping, 0-80 (default 16)");
    g_pingArgs.interval = arg_int0("i", "interval", "<ms>", "pause between pings (default 0)");
    g_pingArgs.timeout = arg_int0("W", "timeout", "<ms>", "reply timeout (default 3000)");
    g_pingArgs.drop = arg_int0(nullptr, "drop", "<pct>", "inject: remote ignores this % of pings");
    g_pingArgs.corruptUp =
        arg_int0(nullptr, "corrupt-up", "<pct>", "inject: corrupt this % of requests");
    g_pingArgs.corruptDown =
        arg_int0(nullptr, "corrupt-down", "<pct>", "inject: remote corrupts this % of replies");
    g_pingArgs.quiet = arg_lit0("q", "quiet", "print the summary only");
    g_pingArgs.end = arg_end(4);

    g_streamArgs.dir = arg_str1(nullptr, nullptr, "<tx|rx>", "tx: base -> remote, rx: remote -> base");
    g_streamArgs.count = arg_int0("n", "count", "<n>", "packets, 1-1024 (default 50)");
    g_streamArgs.size = arg_int0("s", "size", "<bytes>", "data per packet, 0-117 (default 32)");
    g_streamArgs.gap = arg_int0("g", "gap", "<ms>", "pause between packets (default 20)");
    g_streamArgs.end = arg_end(4);

    const struct {
        const char* name;
        const char* help;
        esp_console_cmd_func_t func;
        void* argtable;
    } cmds[] = {
        {"id", "Read PARTNUMBER/PARTVERSION", cmdId, nullptr},
        {"status", "Show chip status byte, MARCSTATE and FIFO levels", cmdStatus, nullptr},
        {"reset", "Reset the CC1120", cmdReset, &g_resetArgs},
        {"strobe", "Send a command strobe", cmdStrobe, &g_strobeArgs},
        {"rd", "Read register(s)", cmdRead, &g_rdArgs},
        {"wr", "Write a register", cmdWrite, &g_wrArgs},
        {"dump", "Dump all configuration registers", cmdDump, nullptr},
        {"preset", "Load the default 434 MHz configuration and calibrate", cmdPreset, nullptr},
        {"freq", "Get or set the carrier frequency", cmdFreq, &g_freqArgs},
        {"send", "Transmit a packet, then return to RX", cmdSend, &g_sendArgs},
        {"recv", "Wait for a received packet", cmdRecv, &g_recvArgs},
        {"cmd", "Send a command to the remote unit and print its reply", cmdCommand, &g_cmdArgs},
        {"ping", "Ping the remote: loss/corruption stats, optional fault injection", cmdPing,
         &g_pingArgs},
        {"stream", "Run a test data stream to (tx) or from (rx) the remote", cmdStream,
         &g_streamArgs},
    };
    for (const auto& c : cmds) {
        if (esp_err_t err = registerCmd(c.name, c.help, c.func, c.argtable); err != ESP_OK) {
            return err;
        }
    }
    return esp_console_register_help_command();
}

}  // namespace

esp_err_t start(cc1120::Cc1120& radio, api::RadioApi& api) {
    g_radio = &radio;
    g_api = &api;

    esp_console_repl_t* repl = nullptr;
    esp_console_repl_config_t replConfig = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    replConfig.prompt = "cc1120>";
    replConfig.task_stack_size = 6144;  // ping/stream keep several frame buffers on the stack
    esp_console_dev_uart_config_t uartConfig = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    if (esp_err_t err = esp_console_new_repl_uart(&uartConfig, &replConfig, &repl); err != ESP_OK) {
        return err;
    }
    if (esp_err_t err = registerCommands(); err != ESP_OK) {
        return err;
    }
    return esp_console_start_repl(repl);
}

}  // namespace console
