#include "console/console.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <strings.h>

#include "argtable3/argtable3.h"
#include "esp_console.h"

#include "cc1120/cc1120_presets.h"

namespace console {

namespace {

using cc1120::Cc1120;

Cc1120* g_radio = nullptr;

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
        if (esp_err_t err = g_radio->setFrequency(uint32_t(std::llround(hz))); err != ESP_OK) {
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
    };
    for (const auto& c : cmds) {
        if (esp_err_t err = registerCmd(c.name, c.help, c.func, c.argtable); err != ESP_OK) {
            return err;
        }
    }
    return esp_console_register_help_command();
}

}  // namespace

esp_err_t start(cc1120::Cc1120& radio) {
    g_radio = &radio;

    esp_console_repl_t* repl = nullptr;
    esp_console_repl_config_t replConfig = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    replConfig.prompt = "cc1120>";
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
