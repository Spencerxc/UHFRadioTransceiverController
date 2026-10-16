#include "cc1120/cc1120.h"

#include <cstring>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

namespace cc1120 {

namespace {

constexpr const char* kTag = "cc1120";

// Holds the driver mutex for the lifetime of the object.
class Lock {
public:
    explicit Lock(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(m_); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    SemaphoreHandle_t m_;
};

bool isExtended(uint16_t addr) { return (addr & 0xFF00) == (kExtAddr << 8); }

// LO divider for an FS_CFG.FSD_BANDSELECT value, or 0 if invalid.
uint32_t loDivider(uint8_t bandSelect) {
    switch (bandSelect) {
        case 0x02: return 4;
        case 0x04: return 8;
        case 0x06: return 12;
        case 0x08: return 16;
        case 0x0A: return 20;
        case 0x0B: return 24;
        default: return 0;
    }
}

}  // namespace

esp_err_t Cc1120::init(const Pins& pins) {
    pins_ = pins;

    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t out{};
    out.pin_bit_mask = (1ULL << pins_.csn) | (1ULL << pins_.resetN);
    out.mode = GPIO_MODE_OUTPUT;
    if (esp_err_t err = gpio_config(&out); err != ESP_OK) {
        return err;
    }
    gpio_set_level(pins_.csn, 1);
    gpio_set_level(pins_.resetN, 1);

    spi_bus_config_t bus{};
    bus.mosi_io_num = pins_.mosi;
    bus.miso_io_num = pins_.miso;
    bus.sclk_io_num = pins_.sclk;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = kMaxXfer;
    if (esp_err_t err = spi_bus_initialize(pins_.host, &bus, SPI_DMA_CH_AUTO); err != ESP_OK) {
        return err;
    }

    spi_device_interface_config_t devcfg{};
    devcfg.mode = 0;
    devcfg.clock_speed_hz = pins_.clockHz;
    devcfg.spics_io_num = -1;  // CSn is handled manually (see class comment)
    devcfg.queue_size = 1;
    if (esp_err_t err = spi_bus_add_device(pins_.host, &devcfg, &dev_); err != ESP_OK) {
        return err;
    }

    if (esp_err_t err = hardReset(); err != ESP_OK) {
        ESP_LOGE(kTag, "chip not ready after reset (check wiring/power): %s", esp_err_to_name(err));
        return err;
    }

    uint8_t part = 0;
    uint8_t version = 0;
    if (esp_err_t err = partNumber(part); err != ESP_OK) {
        return err;
    }
    partVersion(version);
    if (part != kPartNumberCc1120) {
        ESP_LOGE(kTag, "unexpected PARTNUMBER 0x%02X (expected 0x%02X)", part, kPartNumberCc1120);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(kTag, "CC1120 found, PARTVERSION 0x%02X", version);
    return ESP_OK;
}

esp_err_t Cc1120::hardReset() {
    gpio_set_level(pins_.resetN, 0);
    esp_rom_delay_us(100);
    gpio_set_level(pins_.resetN, 1);
    return waitChipReady();
}

esp_err_t Cc1120::softReset() {
    if (esp_err_t err = strobe(SRES); err != ESP_OK) {
        return err;
    }
    return waitChipReady();
}

esp_err_t Cc1120::readReg(uint16_t addr, uint8_t& value) {
    return transfer(addr, true, false, &value, 1, nullptr);
}

esp_err_t Cc1120::writeReg(uint16_t addr, uint8_t value) {
    return transfer(addr, false, false, &value, 1, nullptr);
}

esp_err_t Cc1120::readBurst(uint16_t addr, uint8_t* data, size_t len) {
    return transfer(addr, true, true, data, len, nullptr);
}

esp_err_t Cc1120::writeBurst(uint16_t addr, const uint8_t* data, size_t len) {
    // transfer() only reads from data on a write.
    return transfer(addr, false, true, const_cast<uint8_t*>(data), len, nullptr);
}

esp_err_t Cc1120::strobe(uint8_t cmd, uint8_t* status) {
    if (cmd < SRES || cmd > SNOP) {
        return ESP_ERR_INVALID_ARG;
    }
    return transfer(cmd, false, false, nullptr, 0, status);
}

esp_err_t Cc1120::marcState(uint8_t& state) {
    esp_err_t err = readReg(MARCSTATE, state);
    state &= kMarcStateMask;
    return err;
}

esp_err_t Cc1120::applyPreset(const RegSetting* settings, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (esp_err_t err = writeReg(settings[i].addr, settings[i].value); err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t Cc1120::getFrequency(uint32_t& hz) {
    uint8_t fsCfg = 0;
    if (esp_err_t err = readReg(FS_CFG, fsCfg); err != ESP_OK) {
        return err;
    }
    uint32_t div = loDivider(fsCfg & kBandSelectMask);
    if (div == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t freq[3] = {};
    if (esp_err_t err = readBurst(FREQ2, freq, sizeof(freq)); err != ESP_OK) {
        return err;
    }
    uint64_t word = (uint64_t(freq[0]) << 16) | (uint64_t(freq[1]) << 8) | freq[2];
    // f_RF = FREQ * f_XOSC / (2^16 * LO_DIV)
    hz = uint32_t((word * kXoscHz) / (uint64_t(65536) * div));
    return ESP_OK;
}

esp_err_t Cc1120::setFrequency(uint32_t hz) {
    if (hz < kBandMinHz || hz > kBandMaxHz) {
        return ESP_ERR_INVALID_ARG;
    }
    constexpr uint64_t kDiv = 8;  // 410-480 MHz band
    uint64_t word = (uint64_t(hz) * kDiv * 65536 + kXoscHz / 2) / kXoscHz;

    // Frequency registers must only be changed in IDLE.
    if (esp_err_t err = strobe(SIDLE); err != ESP_OK) {
        return err;
    }
    int64_t start = esp_timer_get_time();
    uint8_t state = 0;
    do {
        if (esp_err_t err = marcState(state); err != ESP_OK) {
            return err;
        }
        if (esp_timer_get_time() - start > kChipReadyTimeoutUs) {
            return ESP_ERR_TIMEOUT;
        }
    } while (state != 0x01);  // MARC_STATE IDLE

    uint8_t fsCfg = 0;
    if (esp_err_t err = readReg(FS_CFG, fsCfg); err != ESP_OK) {
        return err;
    }
    fsCfg = (fsCfg & ~kBandSelectMask) | kBand410_480;
    if (esp_err_t err = writeReg(FS_CFG, fsCfg); err != ESP_OK) {
        return err;
    }

    const uint8_t freq[3] = {uint8_t(word >> 16), uint8_t(word >> 8), uint8_t(word)};
    if (esp_err_t err = writeBurst(FREQ2, freq, sizeof(freq)); err != ESP_OK) {
        return err;
    }
    return strobe(SCAL);
}

esp_err_t Cc1120::startRx(const RegSetting* preset, size_t count) {
    if (esp_err_t err = strobe(SIDLE); err != ESP_OK) {
        return err;
    }
    if (esp_err_t err = applyPreset(preset, count); err != ESP_OK) {
        return err;
    }
    if (esp_err_t err = strobe(SCAL); err != ESP_OK) {
        return err;
    }
    // Wait for calibration to finish (CALIBRATE → IDLE) before entering RX.
    const int64_t start = esp_timer_get_time();
    uint8_t state = 0;
    do {
        if (esp_err_t err = marcState(state); err != ESP_OK) {
            return err;
        }
        if (esp_timer_get_time() - start > kChipReadyTimeoutUs) {
            return ESP_ERR_TIMEOUT;
        }
    } while (state != 0x01);  // MARC_STATE IDLE
    return strobe(SRX);
}

esp_err_t Cc1120::readRxFifo(uint8_t* data, size_t len) {
    return readBurst(kFifo, data, len);
}

esp_err_t Cc1120::writeTxFifo(const uint8_t* data, size_t len) {
    return writeBurst(kFifo, data, len);
}

const char* Cc1120::chipStateName(uint8_t statusByte) {
    switch (static_cast<ChipState>(statusByte & kStatusStateMask)) {
        case ChipState::Idle: return "IDLE";
        case ChipState::Rx: return "RX";
        case ChipState::Tx: return "TX";
        case ChipState::FstxOn: return "FSTXON";
        case ChipState::Calibrate: return "CALIBRATE";
        case ChipState::Settling: return "SETTLING";
        case ChipState::RxFifoError: return "RX_FIFO_ERR";
        case ChipState::TxFifoError: return "TX_FIFO_ERR";
    }
    return "?";
}

const char* Cc1120::marcStateName(uint8_t state) {
    static constexpr const char* kNames[] = {
        "SLEEP",          "IDLE",        "XOFF",          "BIAS_SETTLE_MC",
        "REG_SETTLE_MC",  "MANCAL",      "BIAS_SETTLE",   "REG_SETTLE",
        "STARTCAL",       "BWBOOST",     "FS_LOCK",       "IFADCON",
        "ENDCAL",         "RX",          "RX_END",        "RX_RST",
        "TXRX_SWITCH",    "RX_FIFO_ERR", "FSTXON",        "TX",
        "TX_END",         "RXTX_SWITCH", "TX_FIFO_ERR",   "IFADCON_TXRX",
    };
    state &= kMarcStateMask;
    return state < sizeof(kNames) / sizeof(kNames[0]) ? kNames[state] : "?";
}

esp_err_t Cc1120::transfer(uint16_t addr, bool read, bool burst, uint8_t* data,
                           size_t len, uint8_t* status) {
    if (dev_ == nullptr || mutex_ == nullptr) {
        return ESP_ERR_INVALID_STATE;  // init() failed before SPI was set up
    }
    if (len > kFifoSize || (len > 0 && data == nullptr)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t rw = (read ? kReadBit : 0) | (burst ? kBurstBit : 0);
    size_t hdrLen = 0;
    if (isExtended(addr)) {
        txBuf_[0] = rw | kExtAddr;
        txBuf_[1] = uint8_t(addr & 0xFF);
        hdrLen = 2;
    } else if (addr <= kFifo && addr != kExtAddr) {
        txBuf_[0] = rw | uint8_t(addr);
        hdrLen = 1;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    Lock lock(mutex_);

    if (read) {
        std::memset(txBuf_ + hdrLen, 0, len);
    } else if (len > 0) {
        std::memcpy(txBuf_ + hdrLen, data, len);
    }

    const size_t total = hdrLen + len;
    spi_transaction_t t{};
    t.length = total * 8;
    t.tx_buffer = txBuf_;
    t.rx_buffer = rxBuf_;

    if (esp_err_t err = selectAndWaitReady(); err != ESP_OK) {
        deselect();
        return err;
    }
    esp_err_t err = spi_device_polling_transmit(dev_, &t);
    deselect();
    if (err != ESP_OK) {
        return err;
    }

    if (status != nullptr) {
        *status = rxBuf_[0];
    }
    if (read) {
        std::memcpy(data, rxBuf_ + hdrLen, len);
    }
    return ESP_OK;
}

esp_err_t Cc1120::selectAndWaitReady() {
    gpio_set_level(pins_.csn, 0);
    const int64_t start = esp_timer_get_time();
    while (gpio_get_level(pins_.miso) != 0) {
        if (esp_timer_get_time() - start > kChipReadyTimeoutUs) {
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

void Cc1120::deselect() { gpio_set_level(pins_.csn, 1); }

esp_err_t Cc1120::waitChipReady() {
    if (mutex_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    Lock lock(mutex_);
    esp_err_t err = selectAndWaitReady();
    deselect();
    return err;
}

}  // namespace cc1120
