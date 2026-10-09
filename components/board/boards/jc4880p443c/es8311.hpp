#pragma once

// The ES8311 audio codec of the JC4880P443C and its amplifier (es8311.cpp).

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "hc/board/board.hpp"

namespace hc::board::detail {

// Looks for the codec on the board's I2C bus and sets it up (amplifier off). ESP_ERR_NOT_FOUND
// when it does not answer.
esp_err_t es8311_init(i2c_master_bus_handle_t bus);

// the codec for hc::board::audio_codec(), nullptr when es8311_init() failed
const AudioCodec *es8311();

} // namespace hc::board::detail
