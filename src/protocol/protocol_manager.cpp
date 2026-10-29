#include "protocol/protocol_manager.h"

#include <cstring>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "cc1120/cc1120_presets.h"
#include "rfswitch/rfswitch.h"

namespace protocol {

namespace {

constexpr const char* kTag = "protocol";

// MARCSTATE values (CC1120 datasheet, MARCSTATE register).
constexpr uint8_t kMarcIdle = 0x01;
constexpr uint8_t kMarcRx = 0x0D;
constexpr uint8_t kMarcRxFifoErr = 0x11;
constexpr uint8_t kMarcTxFifoErr = 0x16;

constexpr uint32_t kRxPollMs = 10;
constexpr uint32_t kRetryMs = 1000;
constexpr uint32_t kStateTimeoutMs = 20;
constexpr uint32_t kTxTimeoutMs = 3000;  // 128 bytes at 1.2 kbps is ~0.9 s
constexpr uint32_t kSwitchSettleUs = 100;
constexpr size_t kRxQueueDepth = 4;

class Lock {
public:
    explicit Lock(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(m_); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    SemaphoreHandle_t m_;
};

}  // namespace

esp_err_t ProcessManager::begin(const cc1120::Pins& pins) {
    pins_ = pins;

    lock_ = xSemaphoreCreateMutex();
    rxQueue_ = xQueueCreate(kRxQueueDepth, sizeof(Packet));
    if (lock_ == nullptr || rxQueue_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    // RX path is the safe default, so select it before anything else.
    if (esp_err_t err = rfswitch::init(); err != ESP_OK) {
        return err;
    }

    const esp_err_t first = bringUp(true);
    if (first != ESP_OK) {
        ESP_LOGE(kTag, "radio bring-up failed: %s (will retry)", esp_err_to_name(first));
    } else {
        ESP_LOGI(kTag, "radio ready, RX");
    }

    if (xTaskCreate(taskEntry, "protocol", 4096, this, 5, &task_) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return first;
}

esp_err_t ProcessManager::bringUp(bool fullInit) {
    Lock lock(lock_);
    state_ = State::Init;
    rfswitch::setRx();

    if (fullInit) {
        if (esp_err_t err = radio_.init(pins_); err != ESP_OK) {
            return err;
        }
    } else {
        // The SPI bus is already set up; reset the chip and re-check the part.
        if (esp_err_t err = radio_.hardReset(); err != ESP_OK) {
            return err;
        }
        uint8_t part = 0;
        if (esp_err_t err = radio_.partNumber(part); err != ESP_OK) {
            return err;
        }
        if (part != cc1120::kPartNumberCc1120) {
            return ESP_ERR_NOT_FOUND;
        }
    }

    if (esp_err_t err = radio_.startRx(cc1120::kPreset434MHz, cc1120::kPreset434MHzCount);
        err != ESP_OK) {
        return err;
    }
    state_ = State::Rx;
    return ESP_OK;
}

esp_err_t ProcessManager::waitMarc(uint8_t target, uint32_t timeoutMs) {
    const int64_t deadline = esp_timer_get_time() + int64_t(timeoutMs) * 1000;
    uint8_t marc = 0;
    for (;;) {
        if (esp_err_t err = radio_.marcState(marc); err != ESP_OK) {
            return err;
        }
        if (marc == target) {
            return ESP_OK;
        }
        if (esp_timer_get_time() > deadline) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
}

// Flushes both FIFOs and puts the radio back in RX on the RX RF path.
esp_err_t ProcessManager::enterRx() {
    rfswitch::setRx();
    if (esp_err_t err = radio_.strobe(cc1120::SIDLE); err != ESP_OK) {
        return err;
    }
    if (esp_err_t err = waitMarc(kMarcIdle, kStateTimeoutMs); err != ESP_OK) {
        return err;
    }
    if (esp_err_t err = radio_.strobe(cc1120::SFRX); err != ESP_OK) {
        return err;
    }
    if (esp_err_t err = radio_.strobe(cc1120::SFTX); err != ESP_OK) {
        return err;
    }
    if (esp_err_t err = radio_.strobe(cc1120::SRX); err != ESP_OK) {
        return err;
    }
    state_ = State::Rx;
    return ESP_OK;
}

esp_err_t ProcessManager::transmit(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0 || len > kMaxPayload) {
        return ESP_ERR_INVALID_ARG;
    }
    Lock lock(lock_);
    if (state_ == State::Init) {
        return ESP_ERR_INVALID_STATE;
    }

    // Variable-length packet mode: the first FIFO byte is the payload length.
    uint8_t frame[kMaxPayload + 1];
    frame[0] = uint8_t(len);
    std::memcpy(frame + 1, data, len);

    esp_err_t err = radio_.strobe(cc1120::SIDLE);
    if (err == ESP_OK) err = waitMarc(kMarcIdle, kStateTimeoutMs);
    if (err == ESP_OK) err = radio_.strobe(cc1120::SFTX);
    if (err == ESP_OK) err = radio_.writeTxFifo(frame, len + 1);
    if (err == ESP_OK) {
        // Select the TX path before the PA turns on, and let the switch settle.
        rfswitch::setTx();
        esp_rom_delay_us(kSwitchSettleUs);
        state_ = State::Tx;
        err = radio_.strobe(cc1120::STX);
    }
    if (err == ESP_OK) {
        // TXOFF_MODE defaults to IDLE, so TX is done once MARCSTATE reads IDLE.
        err = waitMarc(kMarcIdle, kTxTimeoutMs);
    }

    // Always return to RX, whether or not the transmit succeeded.
    const esp_err_t rxErr = enterRx();
    if (rxErr != ESP_OK) {
        ESP_LOGE(kTag, "failed to return to RX: %s", esp_err_to_name(rxErr));
        rfswitch::setRx();
        state_ = State::Init;  // the task re-runs bring-up
    }
    return err != ESP_OK ? err : rxErr;
}

esp_err_t ProcessManager::receive(uint8_t* buf, size_t bufLen, size_t& outLen, uint32_t timeoutMs) {
    if (buf == nullptr || rxQueue_ == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    Packet pkt;
    if (xQueueReceive(rxQueue_, &pkt, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (pkt.len > bufLen) {
        return ESP_ERR_INVALID_SIZE;
    }
    std::memcpy(buf, pkt.data, pkt.len);
    outLen = pkt.len;
    return ESP_OK;
}

esp_err_t ProcessManager::setFrequency(uint32_t hz) {
    Lock lock(lock_);
    if (state_ == State::Init) {
        return ESP_ERR_INVALID_STATE;
    }
    // Cc1120::setFrequency leaves the chip in IDLE with SCAL running.
    esp_err_t err = radio_.setFrequency(hz);
    if (err == ESP_OK) err = waitMarc(kMarcIdle, kStateTimeoutMs);
    const esp_err_t rxErr = enterRx();
    if (rxErr != ESP_OK) {
        state_ = State::Init;
    }
    return err != ESP_OK ? err : rxErr;
}

esp_err_t ProcessManager::frequency(uint32_t& hz) {
    Lock lock(lock_);
    return radio_.getFrequency(hz);
}

void ProcessManager::pollRx() {
    uint8_t marc = 0;
    uint8_t nBytes = 0;
    if (radio_.marcState(marc) != ESP_OK || radio_.readReg(cc1120::NUM_RXBYTES, nBytes) != ESP_OK) {
        state_ = State::Init;  // SPI trouble: force a bring-up retry
        return;
    }

    if (marc == kMarcRxFifoErr || marc == kMarcTxFifoErr) {
        ESP_LOGW(kTag, "FIFO error, restarting RX");
        if (enterRx() != ESP_OK) state_ = State::Init;
        return;
    }
    if (marc == kMarcRx) {
        return;  // still listening (or a packet is mid-reception)
    }

    // RXOFF_MODE defaults to IDLE, so the chip leaves RX once a packet has been
    // fully received; the whole packet is then in the FIFO.
    if (nBytes > 0) {
        uint8_t len = 0;
        Packet pkt{};
        if (radio_.readRxFifo(&len, 1) == ESP_OK && len > 0 && len <= kMaxPayload &&
            nBytes >= len + 1u && radio_.readRxFifo(pkt.data, len) == ESP_OK) {
            pkt.len = len;
            xQueueSend(rxQueue_, &pkt, 0);  // drop if the application is not keeping up
        }
    }
    // Flush any leftover/status bytes and resume listening.
    if (enterRx() != ESP_OK) state_ = State::Init;
}

void ProcessManager::taskEntry(void* arg) { static_cast<ProcessManager*>(arg)->run(); }

void ProcessManager::run() {
    for (;;) {
        if (state_ == State::Init) {
            if (bringUp(false) == ESP_OK) {
                ESP_LOGI(kTag, "radio ready, RX");
            } else {
                vTaskDelay(pdMS_TO_TICKS(kRetryMs));
            }
            continue;
        }
        {
            Lock lock(lock_);
            if (state_ == State::Rx) {
                pollRx();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(kRxPollMs));
    }
}

}  // namespace protocol
