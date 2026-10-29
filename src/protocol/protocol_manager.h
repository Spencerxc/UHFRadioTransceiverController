#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cc1120/cc1120.h"

namespace protocol {

enum class State : uint8_t { Init, Rx, Tx };

// Largest payload per packet. The first FIFO byte is the length byte
// (variable-length packet mode), so one packet is at most kFifoSize - 1 bytes.
constexpr size_t kMaxPayload = cc1120::Cc1120::kFifoSize - 1;

// Protocol Process Manager (1U1SMGR1).
//
// Translates send/receive/config requests from the Interface API into CC1120
// register and strobe operations executed through the SPI Device Driver, and
// drives the RF switch. It owns the radio state machine: the radio sits in RX
// whenever it is not transmitting, and always returns to RX after a transmit,
// including after an error. A background task keeps the radio in RX, drains the
// RX FIFO into a queue, and retries bring-up if the chip was not found at boot,
// so an unattended unit recovers on its own.
class ProcessManager {
public:
    // Initialises the RF switch (RX path), the SPI driver and the radio, then
    // starts the RX task. Returns the first bring-up error, but the task keeps
    // retrying, so a non-OK result is not fatal.
    esp_err_t begin(const cc1120::Pins& pins);

    // Transmits one packet (1..kMaxPayload bytes) and returns to RX.
    esp_err_t transmit(const uint8_t* data, size_t len);

    // Pops a received payload. Returns ESP_ERR_TIMEOUT if none arrives in time
    // and ESP_ERR_INVALID_SIZE if buf is too small (the packet is discarded).
    esp_err_t receive(uint8_t* buf, size_t bufLen, size_t& outLen, uint32_t timeoutMs);

    // Sets the carrier (410-480 MHz) and returns to RX.
    esp_err_t setFrequency(uint32_t hz);
    esp_err_t frequency(uint32_t& hz);

    State state() const { return state_; }
    bool ready() const { return state_ != State::Init; }

    // Raw access for the debug console.
    cc1120::Cc1120& radio() { return radio_; }

private:
    struct Packet {
        uint8_t len;
        uint8_t data[kMaxPayload];
    };

    static void taskEntry(void* arg);
    void run();

    esp_err_t bringUp(bool fullInit);
    esp_err_t enterRx();  // caller holds lock_
    esp_err_t waitMarc(uint8_t target, uint32_t timeoutMs);
    void pollRx();        // caller holds lock_

    cc1120::Cc1120 radio_;
    cc1120::Pins pins_{};
    SemaphoreHandle_t lock_ = nullptr;  // serialises TX / config against the RX task
    QueueHandle_t rxQueue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    volatile State state_ = State::Init;
};

}  // namespace protocol
