#include "api/radio_api.h"

namespace api {

Status RadioApi::status() {
    Status s{};
    s.ready = manager_.ready();
    s.state = manager_.state();
    if (!s.ready || manager_.frequency(s.frequencyHz) != ESP_OK) {
        s.frequencyHz = 0;
    }
    return s;
}

}  // namespace api
