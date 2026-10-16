#include "hc/relay/relay.hpp"

#include "driver/gpio.h"
#include "esp_log.h"

namespace hc::relay {

namespace {

constexpr const char *kTag = "relay";

// the GPIO of relay n+1, -1: none; written once by start() before the tasks that call set()
int g_gpio[kMaxRelays] = {-1, -1, -1, -1};

} // namespace

esp_err_t start(const signed char *gpios, const bool *pullups, size_t count)
{
    esp_err_t result = ESP_OK;
    for (size_t n = 0; n < kMaxRelays && n < count; n++) {
        if (gpios[n] < 0) {
            continue;
        }
        const int gpio = gpios[n];
        if (!GPIO_IS_VALID_OUTPUT_GPIO(gpio)) {
            ESP_LOGW(kTag, "relay %u: GPIO %d cannot be an output", static_cast<unsigned>(n + 1), gpio);
            result = ESP_ERR_INVALID_ARG;
            continue;
        }
        // the off level before it becomes an output: the relay must not switch while the board
        // starts. A relay on the pullup is closed by default: released (high) until Home Assistant answers.
        const bool pullup = pullups[n];
        gpio_set_level(static_cast<gpio_num_t>(gpio), pullup ? 1 : 0);
        gpio_config_t io{};
        io.pin_bit_mask = 1ULL << gpio;
        // input too, so is_on() reads what the pin really drives; on the pullup only low is driven
        io.mode = pullup ? GPIO_MODE_INPUT_OUTPUT_OD : GPIO_MODE_INPUT_OUTPUT;
        io.pull_up_en = pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
        if (gpio_config(&io) != ESP_OK) {
            ESP_LOGW(kTag, "relay %u: GPIO %d did not configure", static_cast<unsigned>(n + 1), gpio);
            result = ESP_FAIL;
            continue;
        }
        g_gpio[n] = gpio;
        ESP_LOGI(kTag, "relay %u on GPIO %d%s", static_cast<unsigned>(n + 1), gpio, pullup ? ", pullup" : "");
    }
    return result;
}

bool running()
{
    for (const int gpio : g_gpio) {
        if (gpio >= 0) {
            return true;
        }
    }
    return false;
}

int gpio_of(unsigned relay)
{
    return relay >= 1 && relay <= kMaxRelays ? g_gpio[relay - 1] : -1;
}

bool set(unsigned relay, bool on)
{
    const int gpio = gpio_of(relay);
    if (gpio < 0) {
        return false;
    }
    gpio_set_level(static_cast<gpio_num_t>(gpio), on ? 1 : 0);
    return true;
}

bool is_on(unsigned relay)
{
    const int gpio = gpio_of(relay);
    return gpio >= 0 && gpio_get_level(static_cast<gpio_num_t>(gpio)) != 0;
}

} // namespace hc::relay
