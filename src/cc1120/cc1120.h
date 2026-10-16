#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "cc1120/cc1120_regs.h"

namespace cc1120 {

struct RegSetting {
    uint16_t addr;
    uint8_t value;
};

struct Pins {
    spi_host_device_t host;
    gpio_num_t sclk;
    gpio_num_t miso;
    gpio_num_t mosi;
    gpio_num_t csn;
    gpio_num_t resetN;
    int clockHz;
};

// Driver for the TI CC1120 over SPI.
//
// CSn is driven manually: after pulling CSn low the CC1120 holds SO (MISO)
// high until its crystal is stable (CHIP_RDYn), and no clock may be sent
// before SO goes low. Every SPI transaction is guarded by a mutex so the
// driver can be shared between FreeRTOS tasks.
class Cc1120 {
public:
    static constexpr size_t kFifoSize = 128;
    // Reference oscillator on the CC1120EM boards. Change if your board differs.
    static constexpr uint32_t kXoscHz = 32'000'000;
    static constexpr uint32_t kBandMinHz = 410'000'000;
    static constexpr uint32_t kBandMaxHz = 480'000'000;

    // Initialises the SPI bus, GPIOs and mutex, hard-resets the chip and
    // checks PARTNUMBER. Returns ESP_ERR_NOT_FOUND on a part-number mismatch.
    esp_err_t init(const Pins& pins);

    esp_err_t hardReset();
    esp_err_t softReset();

    esp_err_t readReg(uint16_t addr, uint8_t& value);
    esp_err_t writeReg(uint16_t addr, uint8_t value);
    esp_err_t readBurst(uint16_t addr, uint8_t* data, size_t len);
    esp_err_t writeBurst(uint16_t addr, const uint8_t* data, size_t len);

    // Sends a command strobe and returns the chip status byte.
    esp_err_t strobe(uint8_t cmd, uint8_t* status = nullptr);
    esp_err_t status(uint8_t& statusByte) { return strobe(SNOP, &statusByte); }
    esp_err_t marcState(uint8_t& state);
    esp_err_t partNumber(uint8_t& part) { return readReg(PARTNUMBER, part); }
    esp_err_t partVersion(uint8_t& version) { return readReg(PARTVERSION, version); }

    esp_err_t applyPreset(const RegSetting* settings, size_t count);

    // Applies a preset, calibrates, and issues SRX. Leaves the chip in RX mode.
    esp_err_t startRx(const RegSetting* preset, size_t count);

    // Carrier frequency, computed from FS_CFG band select and FREQ2/1/0.
    esp_err_t getFrequency(uint32_t& hz);
    // Sets the carrier in the 410-480 MHz band. Leaves the chip in IDLE and
    // runs SCAL so the synthesizer is recalibrated for the new frequency.
    esp_err_t setFrequency(uint32_t hz);

    esp_err_t readRxFifo(uint8_t* data, size_t len);
    esp_err_t writeTxFifo(const uint8_t* data, size_t len);

    static const char* chipStateName(uint8_t statusByte);
    static const char* marcStateName(uint8_t state);

private:
    // Header (up to 2 bytes) + a full FIFO.
    static constexpr size_t kMaxXfer = 2 + kFifoSize;
    static constexpr int64_t kChipReadyTimeoutUs = 10 * 1000;

    esp_err_t transfer(uint16_t addr, bool read, bool burst, uint8_t* data,
                       size_t len, uint8_t* status);
    esp_err_t selectAndWaitReady();
    void deselect();
    esp_err_t waitChipReady();

    Pins pins_{};
    spi_device_handle_t dev_ = nullptr;
    SemaphoreHandle_t mutex_ = nullptr;

    // DMA buffers; only touched while holding mutex_.
    alignas(4) uint8_t txBuf_[kMaxXfer] = {};
    alignas(4) uint8_t rxBuf_[kMaxXfer] = {};
};

}  // namespace cc1120
