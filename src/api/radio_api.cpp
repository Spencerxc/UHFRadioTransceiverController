#include "api/radio_api.h"

#include "esp_timer.h"

namespace api {

Status RadioApi::status() {
    Status s{};
    s.ready = manager_.ready();
    s.state = manager_.state();
    if (!s.ready || manager_.frequency(s.frequencyHz) != ESP_OK) {
        s.frequencyHz = 0;
    }
    s.rxPackets = manager_.rxCount();
    s.txPackets = manager_.txCount();
    s.rxDropped = manager_.rxDropped();
    s.uptimeMs = uint32_t(esp_timer_get_time() / 1000);
    return s;
}

}  // namespace api
