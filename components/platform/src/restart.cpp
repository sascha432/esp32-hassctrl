#include "hc/platform/restart.hpp"

#include <cstdint>

#include "esp_system.h"
#include "hal/wdt_hal.h"
#include "soc/rtc.h"

namespace hc::platform {

void restart()
{
    // as esp_restart_noos() arms it for the last step, here for the shutdown handlers before it
    // (the startup turned it off, nothing else uses it)
    wdt_hal_context_t wdt;
    wdt_hal_init(&wdt, WDT_RWDT, 0, false);
    const auto ticks = static_cast<uint32_t>(uint64_t{kRestartDeadlineMs} * rtc_clk_slow_freq_get_hz() / 1000);
    wdt_hal_write_protect_disable(&wdt);
    wdt_hal_config_stage(&wdt, WDT_STAGE0, ticks, WDT_STAGE_ACTION_RESET_SYSTEM);
    wdt_hal_enable(&wdt);
    wdt_hal_write_protect_enable(&wdt);
    esp_restart();
}

} // namespace hc::platform
