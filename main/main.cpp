// The panel firmware: composition root. It creates and wires the components and contains no
// logic of its own (docs/architecture.md section 3).

#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_ota_ops.h"
#include "hc/board/board.hpp"
#include "hc/config/dashboard.hpp"
#include "hc/dashboard/dashboard.hpp"
#include "hc/devconsole/devconsole.hpp"
#include "hc/frame_display/frame_display.hpp"
#include "hc/hass/hass.hpp"
#include "hc/light_sensor/ldr.hpp"
#include "hc/light_sensor/light_sensor.hpp"
#include "hc/memory/large.hpp"
#include "hc/network/network.hpp"
#include "hc/orientation/orientation.hpp"
#include "hc/platform/device.hpp"
#include "hc/platform/storage.hpp"
#include "hc/platform/system_info.hpp"
#include "hc/platform/time_sync.hpp"
#include "hc/ui/ui.hpp"
#include "hc/uidebug/uidebug.hpp"
#include "hc/webserver/webserver.hpp"
#include "soc/soc_caps.h"
#include "ui_backend.hpp"

namespace {

constexpr const char *kTag = "main";

// LVGL's log (warnings and LV_LOG_USER) into the ESP-IDF log, without LVGL's trailing newline
void lvgl_log(const char *text)
{
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) {
        length--;
    }
    ESP_LOGI("lvgl", "%.*s", static_cast<int>(length), text);
}

// Hands the board's panel to esp_lvgl_port, which runs the LVGL task (docs/architecture.md
// section 8). Returns nullptr when LVGL cannot be started.
lv_disp_t *start_lvgl(const hc::board::Handles &handles)
{
    const auto &info = hc::board::info();

#if LVGL_VERSION_MAJOR == 8
    if (info.swap_bytes != (LV_COLOR_16_SWAP != 0)) {
        ESP_LOGE(kTag, "CONFIG_LV_COLOR_16_SWAP does not match the board, check boards/<board>/sdkconfig.defaults");
        return nullptr;
    }
#endif

    const lvgl_port_cfg_t port_config = ESP_LVGL_PORT_INIT_CONFIG();
    if (lvgl_port_init(&port_config) != ESP_OK) {
        ESP_LOGE(kTag, "esp_lvgl_port could not start");
        return nullptr;
    }
    lv_log_register_print_cb(lvgl_log);

    lv_disp_t *disp = nullptr;
    if (info.full_frames) {
        // whole frames only: hc::frame_display renders into PSRAM and rotates in software
        const hc::frame_display::Config frame_config{.io = handles.io,
                                                     .panel = handles.panel,
                                                     .width = info.width,
                                                     .height = info.height,
                                                     .band_lines = info.draw_buffer_lines};
        if (lvgl_port_lock(0)) {
            disp = hc::frame_display::add(frame_config);
            lvgl_port_unlock();
        }
#if SOC_MIPI_DSI_SUPPORTED
    } else if (info.mipi_dsi) {
        // the panel scans its frame buffer: two partial buffers in internal RAM, which draw_bitmap
        // copies into it; no rotation in the panel, so LVGL rotates the bands in software
        lvgl_port_display_cfg_t display_config{};
        display_config.io_handle = handles.io;
        display_config.panel_handle = handles.panel;
        display_config.hres = info.width;
        display_config.vres = info.height;
        display_config.buffer_size = static_cast<uint32_t>(info.width) * info.draw_buffer_lines;
        display_config.double_buffer = true;
        display_config.flags.buff_dma = true;
        display_config.flags.sw_rotate = true;
        const lvgl_port_display_dsi_cfg_t dsi_config{};
        disp = lvgl_port_add_disp_dsi(&display_config, &dsi_config);
#endif
    } else {
        lvgl_port_display_cfg_t display_config{};
        display_config.io_handle = handles.io;
        display_config.panel_handle = handles.panel;
        display_config.hres = info.width;
        display_config.vres = info.height;
        display_config.rotation.swap_xy = info.swap_xy;
        display_config.rotation.mirror_x = info.mirror_x;
        display_config.rotation.mirror_y = info.mirror_y;
        // two partial buffers in internal DMA RAM: LVGL renders one while the other is sent
        display_config.buffer_size = static_cast<uint32_t>(info.width) * info.draw_buffer_lines;
        display_config.double_buffer = true;
        display_config.flags.buff_dma = true;
        disp = lvgl_port_add_disp(&display_config);
    }
    if (disp != nullptr && handles.touch != nullptr) {
        lvgl_port_touch_cfg_t touch_config{};
        touch_config.disp = disp;
        touch_config.handle = handles.touch;
        if (lvgl_port_add_touch(&touch_config) == nullptr) {
            ESP_LOGW(kTag, "touch could not be added to LVGL");
        }
    }
    return disp;
}

// The orientation sensor turns the display unless the rotation is locked in Settings. Until the
// UI has its mailbox (docs/architecture.md section 4) the UI is called under the LVGL lock
// (0: waits as long as it takes).
void on_rotation(hc::orientation::Rotation rotation, void *)
{
    if (lvgl_port_lock(0)) {
        hc::ui::sensor_rotation(static_cast<uint8_t>(rotation));
        lvgl_port_unlock();
    }
}

// The Home Assistant connection for the web UI (the web server does not depend on it)
const char *hass_state_key(hc::hass::State state)
{
    using hc::hass::State;
    switch (state) {
    case State::NotSetUp:
        return "not_set_up";
    case State::WaitingForNetwork:
        return "waiting_for_network";
    case State::Connecting:
        return "connecting";
    case State::Connected:
        return "connected";
    case State::TokenRejected:
        return "token_rejected";
    case State::Disconnected:
        return "disconnected";
    }
    return "?";
}

const hc::webserver::HassAccess kHassAccess{
    .status =
        [](hc::webserver::HassStatus &out) {
            const hc::hass::Status s = hc::hass::status();
            out.state = hass_state_key(s.state);
            snprintf(out.url, sizeof(out.url), "%s", s.url);
            out.token_set = s.token_set;
            snprintf(out.version, sizeof(out.version), "%s", s.version);
            snprintf(out.error, sizeof(out.error), "%s", s.error);
            out.retry_in_s = s.retry_in_s;
            out.connected_s = s.connected_s;
        },
    .set = hc::hass::set_config,
    .reconnect = hc::hass::reconnect,
    .discover =
        [](hc::webserver::HassFound *out, size_t max) {
            hc::hass::Found found[6];
            const size_t count = hc::hass::discover(found, max < 6 ? max : 6, 2000);
            for (size_t i = 0; i < count; i++) {
                // the precision keeps GCC's truncation check from assuming the whole array
                snprintf(out[i].name, sizeof(out[i].name), "%.*s", static_cast<int>(sizeof(out[i].name) - 1),
                         found[i].name);
                snprintf(out[i].url, sizeof(out[i].url), "%.*s", static_cast<int>(sizeof(out[i].url) - 1),
                         found[i].url);
                snprintf(out[i].version, sizeof(out[i].version), "%.*s",
                         static_cast<int>(sizeof(out[i].version) - 1), found[i].version);
            }
            return count;
        },
};

// The dashboard for the web UI (the web server does not depend on it)
hc::webserver::DashboardResult to_web(hc::dashboard::Result result)
{
    using hc::dashboard::Result;
    using hc::webserver::DashboardResult;
    switch (result) {
    case Result::Ok:
        return DashboardResult::Ok;
    case Result::Conflict:
        return DashboardResult::Conflict;
    case Result::Invalid:
        return DashboardResult::Invalid;
    case Result::Failed:
        return DashboardResult::Failed;
    }
    return DashboardResult::Failed;
}

void to_info(const hc::dashboard::ImportReport &report, hc::webserver::DashboardInfo &out)
{
    out.revision = report.revision;
    out.pages = report.pages;
    out.tiles = report.tiles;
    std::snprintf(out.other_keys, sizeof(out.other_keys), "%s", report.other_keys);
}

const hc::webserver::DashboardAccess kDashboardAccess{
    .export_json =
        [](size_t &length, char *error, size_t error_size) -> char * {
            char *text = hc::dashboard::export_json(length);
            if (text == nullptr && error_size > 0) {
                std::snprintf(error, error_size, "%s", hc::dashboard::status().error);
            }
            return text;
        },
    .export_yaml = hc::dashboard::export_yaml,
    .release = hc::dashboard::release,
    .save_json =
        [](const char *text, size_t length, uint32_t expected_revision, hc::webserver::DashboardInfo &out,
           char *error, size_t error_size) {
            uint32_t revision = 0;
            const auto result = hc::dashboard::save_json(std::string_view(text, length), expected_revision,
                                                         revision, error, error_size);
            const auto status = hc::dashboard::status();
            out = {};
            out.revision = revision;
            out.pages = status.pages;
            out.tiles = status.tiles;
            return to_web(result);
        },
    .import_yaml =
        [](const hc::webserver::DashboardFile *files, size_t count, bool check_only,
           hc::webserver::DashboardInfo &out, char *error, size_t error_size) {
            hc::memory::Vector<hc::config::File> uploaded;
            for (size_t i = 0; i < count; i++) {
                uploaded.push_back({std::string_view(files[i].path, files[i].path_length),
                                    std::string_view(files[i].text, files[i].text_length)});
            }
            hc::dashboard::ImportReport report{};
            const auto result = hc::dashboard::import_yaml(uploaded, check_only, report, error, error_size);
            out = {};
            to_info(report, out);
            return to_web(result);
        },
};

} // namespace

extern "C" void app_main()
{
    // the dashboard and the trees of an import live in PSRAM (docs/architecture.md section 11)
    hc::memory::set_large_heap(
        [](size_t size) {
            return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
        },
        [](void *pointer) { heap_caps_free(pointer); });
    hc::platform::log_system_info();
    ESP_ERROR_CHECK(hc::platform::init_storage());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
#if CONFIG_HC_DEV_CONSOLE
    // early, while the internal RAM is in one piece; it takes commands after ready() below
    if (hc::devconsole::start() != ESP_OK) {
        ESP_LOGW(kTag, "development console did not start");
    }
#endif

    const auto &board = hc::board::info();
    ESP_LOGI(kTag, "board %s, panel %ux%u, touch %s", board.name, board.width, board.height,
             board.has_touch ? "yes" : "no");

    hc::board::Handles handles{};
    if (hc::board::init(handles) == ESP_OK) {
        lv_disp_t *disp = start_lvgl(handles);
        const bool imu = disp != nullptr && board.imu_address != 0 && handles.i2c != nullptr &&
                         hc::orientation::start(handles.i2c, board.imu_address, board.imu_axes, board.imu_offset, on_rotation, nullptr) == ESP_OK;
        // optional per device: automatic brightness when one answers
        const bool bh1750 = handles.i2c != nullptr &&
                            hc::light_sensor::start(handles.i2c, board.light_sensor_scale) == ESP_OK;
        if (!bh1750 && (board.light_ldr_gpio < 0 || hc::light_sensor::ldr::start(board.light_ldr_gpio) != ESP_OK)) {
            ESP_LOGI(kTag, "no light sensor");
        }
        if (disp != nullptr && lvgl_port_lock(0)) {
            static const hc::ui::BacklightControl backlight{
                .set =
                    [](uint8_t percent, bool perceptual) {
                        hc::board::set_backlight(percent, perceptual ? hc::board::BacklightCurve::Perceptual
                                                                     : hc::board::BacklightCurve::Linear);
                    },
                .set_duty = hc::board::set_backlight_duty,
                .set_minimum = hc::board::set_backlight_minimum,
                .minimum = hc::board::backlight_minimum,
                .set_frequency = [](uint32_t hz) { return hc::board::set_backlight_frequency(hz) == ESP_OK; },
                .max_duty = hc::board::backlight_max_duty(),
                .frequency_hz = board.backlight_hz,
            };
            // the rotation comes from the sensor or from Settings, not from the test screen
            hc::ui::start(disp, ui_backend(),
                          {.board_name = board.name,
                           .cycle_rotation = false,
                           .backlight = &backlight,
                           .network_text = nullptr},
                          imu, {.slide_pages = board.ui_slide_pages,
                           .panel_dialogs = board.ui_panel_dialogs,
                           .large_fonts = board.ui_large_fonts});
#if CONFIG_HC_DEV_CONSOLE
            hc::uidebug::init(disp);
#endif
            // the UI sets the backlight (stored brightness, dimming, standby)
            lvgl_port_unlock();
        }
    } else {
        ESP_LOGW(kTag, "no display");
    }

    if (hc::network::start(hc::platform::device_name(), nullptr, nullptr) != ESP_OK) {
        ESP_LOGE(kTag, "network did not start");
    } else {
        hc::platform::start_time_sync();
        if (hc::dashboard::start() != ESP_OK) {
            ESP_LOGE(kTag, "dashboard did not start");
        }
        if (hc::webserver::start({.device_name = hc::platform::device_name(),
                                  .hass = &kHassAccess,
                                  .dashboard = &kDashboardAccess}) != ESP_OK) {
            ESP_LOGE(kTag, "web server did not start");
        }
        if (hc::hass::start() != ESP_OK) {
            ESP_LOGE(kTag, "Home Assistant connection did not start");
        }
    }

    // After an update the bootloader only keeps this firmware if it confirms itself: until then a
    // reset or a power loss starts the recovery app. It confirms as soon as the screen, the
    // network and the web server run, so an update can be repeated from here; later steps add the
    // checks that prove the firmware works (docs/ui/web.md section 8). The development console,
    // created at the start, takes commands from here on.
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "could not confirm the firmware: %s", esp_err_to_name(err));
    }

#if CONFIG_HC_DEV_CONSOLE
    hc::devconsole::ready();
#endif
}
