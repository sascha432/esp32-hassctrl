#include "bench_mode.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string_view>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hc/board/board.hpp"
#include "hc/config/dashboard.hpp"
#include "hc/memory/large.hpp"
#include "hc/platform/restart.hpp"
#include "hc/sample/sample_hass.hpp"
#include "hc/sample/stub_backend.hpp"
#include "hc/ui/bench.hpp"
#include "hc/uidebug/uidebug.hpp"
#include "sdkconfig.h"

// bench/dashboard/hass.yaml and bench/VERSION, built in (main/CMakeLists.txt), each ending in a 0
extern const char kDashboardYaml[] asm("_binary_hass_yaml_start");
extern const char kBenchVersion[] asm("_binary_VERSION_start");

namespace bench_mode {

namespace {

constexpr const char *kTag = "bench";
// "BENC": `bench start` asked for benchmark mode
constexpr uint32_t kRequest = 0x42454e43;
// the time every run starts at (Mon 2 Feb 2026, 10:08:30 UTC): the clocks show the same hands
constexpr int64_t kStartTime = 1770026910;
// a run takes about two minutes; this is the most `bench run` waits for it
constexpr uint32_t kRunTimeoutMs = 5 * 60 * 1000;

// survives the restart of `bench start`, not a power cycle
RTC_NOINIT_ATTR uint32_t s_request;

struct State {
    bool checked = false;
    bool active = false;
    lv_disp_t *disp = nullptr;
    // the result lines, printed when the run ended (printing in between would stall the LVGL task)
    hc::memory::String result;
    volatile bool done = false;
};

State g;

// the version of the benchmark data without its line end
std::string_view data_version()
{
    std::string_view v(kBenchVersion);
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) {
        v.remove_suffix(1);
    }
    return v;
}

bool load_dashboard()
{
    const hc::config::File files[] = {{"hass.yaml", std::string_view(kDashboardYaml)}};
    auto imported = hc::config::import_yaml(files, "hass.yaml");
    if (!imported) {
        ESP_LOGE(kTag, "benchmark dashboard: %s", imported.error().c_str());
        return false;
    }
    hc::sample::set_dashboard(std::make_shared<const hc::core::Layout>(std::move(imported->layout)));
    return true;
}

void memory_members(char *out, size_t size)
{
    std::snprintf(out, size,
                  "\"internal_free\":%u,\"internal_largest\":%u,\"dma_free\":%u,\"psram_free\":%u,"
                  "\"psram_largest\":%u,\"lvgl_stack_free\":%u",
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
                  // called in the LVGL task: its own stack
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

const hc::ui::bench::Hooks kHooks{
    .now_us = [] { return static_cast<uint64_t>(esp_timer_get_time()); },
    .touch = hc::uidebug::touch,
    .memory = memory_members,
    .line =
        [](const char *text) {
            g.result += text;
            g.result += '\n';
        },
    .done = [] { g.done = true; },
};

int cmd_bench(int argc, char **argv)
{
    const char *verb = argc > 1 ? argv[1] : "";
    if (std::strcmp(verb, "start") == 0) {
        std::printf("restarting into benchmark mode\n");
        std::fflush(stdout);
        s_request = kRequest;
        vTaskDelay(pdMS_TO_TICKS(100));
        hc::platform::restart();
    }
    if (std::strcmp(verb, "run") != 0) {
        std::printf("benchmark mode %s, data version %.*s, scenes version %d\n", g.active ? "on" : "off",
                    static_cast<int>(data_version().size()), data_version().data(), hc::ui::bench::kScenesVersion);
        std::printf("bench start: restart into benchmark mode; bench run: run the scenes (docs/benchmark.md)\n");
        return 0;
    }
    if (!g.active || g.disp == nullptr) {
        std::printf("not in benchmark mode: bench start first\n");
        return 1;
    }
    const auto &board = hc::board::info();
    const esp_app_desc_t *app = esp_app_get_description();
    char header[320];
    std::snprintf(header, sizeof(header),
                  "\"board\":\"%s\",\"variant\":\"%s\",\"firmware\":\"%s\",\"idf\":\"%s\",\"cpu_mhz\":%d,"
                  "\"data\":\"%.*s\"",
                  board.name, board.variant != nullptr ? board.variant : "", app->version, app->idf_ver,
                  CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, static_cast<int>(data_version().size()), data_version().data());
    g.result.clear();
    g.done = false;
    // nothing else on the serial port while the scenes run: the restart after the benchmark brings
    // back the log level of the console's `log` command
    esp_log_level_set("*", ESP_LOG_ERROR);
    bool started = false;
    if (lvgl_port_lock(0)) {
        started = hc::ui::bench::start(g.disp, kHooks, header);
        lvgl_port_unlock();
    }
    if (!started) {
        std::printf("the benchmark did not start (no dashboard shown yet, or a run goes on)\n");
        return 1;
    }
    const int64_t until = esp_timer_get_time() + static_cast<int64_t>(kRunTimeoutMs) * 1000;
    while (!g.done && esp_timer_get_time() < until) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!g.done) {
        std::printf("BENCH-ERROR timeout\n");
        return 1;
    }
    // line by line with a pause: a console on USB drops what does not fit its buffer
    std::printf("BENCH-BEGIN\n");
    unsigned lines = 0;
    std::string_view rest(g.result.c_str(), g.result.size());
    while (!rest.empty()) {
        const size_t end = rest.find('\n');
        const std::string_view line = rest.substr(0, end);
        std::printf("%.*s\n", static_cast<int>(line.size()), line.data());
        std::fflush(stdout);
        lines++;
        rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    std::printf("BENCH-END %u\n", lines);
    g.result = {};
    return 0;
}

} // namespace

bool active()
{
    if (!g.checked) {
        g.checked = true;
        g.active = s_request == kRequest && esp_reset_reason() == ESP_RST_SW;
        s_request = 0;
        if (g.active && !load_dashboard()) {
            g.active = false;
        }
        if (g.active) {
            ESP_LOGW(kTag, "benchmark mode: sample data, no network (docs/benchmark.md)");
        }
    }
    return g.active;
}

const hc::ui::Backend &backend()
{
    static const hc::ui::Backend backend = [] {
        hc::sample::set_verbose(false);
        hc::sample::set_board(hc::board::info().name);
        hc::sample::start_scenario(static_cast<size_t>(hc::sample::find_scenario("connected")));
        hc::sample::set_start_time(kStartTime);
        hc::ui::Backend b = hc::sample::backend();
        b.set_backlight = [](uint8_t percent) {
            hc::sample::backend().set_backlight(percent);
            hc::board::set_backlight(percent);
        };
        b.restart = [] { hc::platform::restart(); };
        return b;
    }();
    return backend;
}

void register_console(lv_disp_t *disp)
{
    g.disp = disp;
    esp_console_cmd_t command{};
    command.command = "bench";
    command.help = "rendering benchmark: restart into benchmark mode, run the scenes (docs/benchmark.md)";
    command.hint = "[start|run]";
    command.func = cmd_bench;
    esp_console_cmd_register(&command);
}

} // namespace bench_mode
