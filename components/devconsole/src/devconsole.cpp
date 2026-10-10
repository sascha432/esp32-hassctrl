#include "hc/devconsole/devconsole.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unistd.h>

#include "sdkconfig.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#else
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#endif
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#if CONFIG_HC_AUDIO
#include "hc/audio/audio.hpp"
#endif
#include "hc/board/board.hpp"
#include "hc/dashboard/dashboard.hpp"
#include "hc/hass/hass.hpp"
#include "hc/light_sensor/ldr.hpp"
#include "hc/logbuf/logbuf.hpp"
#include "hc/orientation/orientation.hpp"
#include "hc/light_sensor/light_sensor.hpp"
#include "hc/network/coprocessor.hpp"
#include "hc/network/network.hpp"
#include "hc/platform/settings.hpp"
#include "hc/platform/storage.hpp"
#include "hc/touch_input/touch_input.hpp"
#include "hc/ui/ui.hpp"
#include "hc/uidebug/uidebug.hpp"
#include "hc/vibration/vibration.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hc/webserver/webserver.hpp"
#include "linenoise/linenoise.h"

#if CONFIG_LIBC_PICOLIBC
// linenoise reads and writes the stdio streams of its task through these (esp_console's own REPL
// task sets them the same way, console_stdio_private.h)
#if !CONFIG_LIBC_PICOLIBC_NEWLIB_COMPATIBILITY
#define tls_stdin linenoise_stdin
#define tls_stdout linenoise_stdout
#endif
extern "C" __thread FILE *tls_stdin;
extern "C" __thread FILE *tls_stdout;
#endif

namespace hc::devconsole {

#if CONFIG_HC_DEV_CONSOLE

namespace {

namespace net = hc::network;

int usage(const char *text)
{
    std::printf("usage: %s\n", text);
    return 1;
}

int check(esp_err_t err)
{
    if (err != ESP_OK) {
        std::printf("error: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

void print_status()
{
    const net::Status s = net::status();
    std::printf("wifi:    %s", net::state_name(s.state));
    if (s.ssid[0] != 0) {
        std::printf(", '%s'", s.ssid);
    }
    if (s.state == net::StaState::Connected) {
        std::printf(", %s, %d dBm, channel %u, connected in %lu ms (%s)", s.ip, s.rssi, s.channel,
                    static_cast<unsigned long>(s.connect_ms), s.connect_fast ? "fast" : "with scan");
    }
    std::printf("\n");
    if (s.error[0] != 0) {
        std::printf("error:   %s\n", s.error);
    }
    std::printf("policy:  %s\n", s.policy == net::Policy::EnteredOrder ? "entered order" : "strongest signal");
    std::printf("hotspot: %s, '%s', password %s, %u clients", s.hotspot_on ? "on" : "off", s.hotspot_ssid,
                s.hotspot_password, s.hotspot_clients);
    if (s.hotspot_off_in_s != 0) {
        std::printf(", off in %lu s", static_cast<unsigned long>(s.hotspot_off_in_s));
    }
    std::printf("%s\n", s.hotspot_enabled ? ", kept on" : "");
}

void print_saved()
{
    net::SavedNetworkInfo saved[net::kMaxNetworks];
    const size_t count = net::saved_networks(saved, net::kMaxNetworks);
    if (count == 0) {
        std::printf("no saved networks\n");
    }
    for (size_t i = 0; i < count; i++) {
        std::printf("%u  %s%s\n", static_cast<unsigned>(i + 1), saved[i].ssid,
                    saved[i].password_needed ? "  (password needed)" : saved[i].open ? "  (open)" : "");
    }
}

void print_scan()
{
    net::ScanEntry entries[20];
    const size_t count = net::last_scan(entries, 20);
    if (count == 0) {
        std::printf("no scan results, run 'wifi scan' first\n");
    }
    for (size_t i = 0; i < count; i++) {
        std::printf("%4d dBm  ch %2u  %s %s%s\n", entries[i].rssi, entries[i].channel, entries[i].secure ? "lock" : "open",
                    entries[i].ssid, entries[i].saved ? "  (saved)" : "");
    }
}

// a 1-based list position from the command line
bool position(const char *text, uint8_t &index)
{
    const int value = std::atoi(text);
    if (value < 1 || value > static_cast<int>(net::kMaxNetworks)) {
        return false;
    }
    index = static_cast<uint8_t>(value - 1);
    return true;
}

int cmd_wifi(int argc, char **argv)
{
    if (argc < 2 || std::strcmp(argv[1], "status") == 0) {
        print_status();
        return 0;
    }
    const char *sub = argv[1];
    uint8_t index = 0;
    if (std::strcmp(sub, "list") == 0) {
        print_saved();
        return 0;
    }
    if (std::strcmp(sub, "add") == 0) {
        if (argc != 4) {
            return usage("wifi add <ssid> <password>   (quote names with spaces)");
        }
        return check(net::add_network(argv[2], argv[3]));
    }
    if (std::strcmp(sub, "connect") == 0) {
        if (argc != 4) {
            return usage("wifi connect <ssid> <password>   (saves it and connects now, like the screen)");
        }
        return check(net::add_network(argv[2], argv[3], true));
    }
    if (std::strcmp(sub, "remove") == 0) {
        if (argc != 3 || !position(argv[2], index)) {
            return usage("wifi remove <position>");
        }
        return check(net::remove_network(index));
    }
    if (std::strcmp(sub, "up") == 0 || std::strcmp(sub, "down") == 0) {
        if (argc != 3 || !position(argv[2], index)) {
            return usage("wifi up|down <position>");
        }
        return check(net::move_network(index, sub[0] == 'u' ? -1 : 1));
    }
    if (std::strcmp(sub, "policy") == 0) {
        if (argc != 3 || (std::strcmp(argv[2], "order") != 0 && std::strcmp(argv[2], "strongest") != 0)) {
            return usage("wifi policy order|strongest");
        }
        return check(net::set_policy(argv[2][0] == 'o' ? net::Policy::EnteredOrder : net::Policy::StrongestSignal));
    }
    if (std::strcmp(sub, "scan") == 0) {
        std::printf("scanning, see 'wifi results' in a few seconds\n");
        return check(net::scan());
    }
    if (std::strcmp(sub, "results") == 0) {
        print_scan();
        return 0;
    }
    if (std::strcmp(sub, "reconnect") == 0) {
        return check(net::reconnect());
    }
    return usage("wifi [status|list|add|connect|remove|up|down|policy|scan|results|reconnect]");
}

// The Wi-Fi coprocessor (ESP32-C6 of the JC4880P443C): its firmware version, and an update from
// a URL. After a successful update this chip restarts, which resets the coprocessor into the new
// firmware.
int cmd_c6(int argc, char **argv)
{
    if (!net::has_coprocessor()) {
        std::printf("this board has no Wi-Fi coprocessor\n");
        return 1;
    }
    if (argc < 2 || std::strcmp(argv[1], "version") == 0) {
        char version[16] = {};
        if (net::coprocessor_version(version, sizeof(version)) != ESP_OK) {
            std::printf("coprocessor does not answer\n");
            return 1;
        }
        std::printf("coprocessor esp_hosted %s, this side %s\n", version, net::host_hosted_version());
        return 0;
    }
    if (std::strcmp(argv[1], "update") == 0 && argc == 3) {
        std::printf("downloading %s\n", argv[2]);
        const esp_err_t err = net::update_coprocessor(argv[2], [](size_t done, size_t total) {
            std::printf("  %u of %u KB\n", static_cast<unsigned>(done / 1024), static_cast<unsigned>(total / 1024));
        });
        if (err != ESP_OK) {
            std::printf("update failed: %s, the coprocessor keeps its firmware\n", esp_err_to_name(err));
            return 1;
        }
        std::printf("done, restarting\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return 0;
    }
    return usage("c6 [version|update <http url>]");
}

int cmd_hotspot(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "on") == 0) {
        return check(net::set_hotspot(true));
    }
    if (argc == 2 && std::strcmp(argv[1], "off") == 0) {
        return check(net::set_hotspot(false));
    }
    if (argc == 2 && std::strcmp(argv[1], "password") == 0) {
        return check(net::new_hotspot_password());
    }
    return usage("hotspot on|off|password");
}

int cmd_fs(int, char **)
{
    const auto &s = hc::platform::storage_status();
    switch (s.fs) {
    case hc::platform::FsState::Mounted:
        std::printf("LittleFS at %s: %zu of %zu KB used\n", hc::platform::kFsMountPoint, s.fs_used / 1024,
                    s.fs_total / 1024);
        break;
    case hc::platform::FsState::MountFailed:
        std::printf("LittleFS cannot be mounted (not formatted automatically)\n");
        break;
    case hc::platform::FsState::NoPartition:
        std::printf("no storage partition\n");
        break;
    }
    return 0;
}

// ------------------------------------------------------------------- screen (uidebug)

// Base64 lines of 76 characters on stdout; carries up to two bytes between the pieces.
struct Base64Out {
    uint8_t carry[3];
    size_t carried = 0;
    size_t column = 0;
    size_t total = 0;

    void emit(const uint8_t *in, size_t n)
    {
        static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        char out[4];
        const uint32_t v = (in[0] << 16) | ((n > 1 ? in[1] : 0) << 8) | (n > 2 ? in[2] : 0);
        out[0] = kAlphabet[(v >> 18) & 63];
        out[1] = kAlphabet[(v >> 12) & 63];
        out[2] = n > 1 ? kAlphabet[(v >> 6) & 63] : '=';
        out[3] = n > 2 ? kAlphabet[v & 63] : '=';
        std::fwrite(out, 1, 4, stdout);
        column += 4;
        if (column >= 76) {
            std::fputc('\n', stdout);
            column = 0;
        }
    }

    void write(const uint8_t *data, size_t size)
    {
        total += size;
        for (size_t i = 0; i < size; i++) {
            carry[carried++] = data[i];
            if (carried == 3) {
                emit(carry, 3);
                carried = 0;
            }
        }
    }

    void finish()
    {
        if (carried > 0) {
            emit(carry, carried);
        }
        if (column > 0) {
            std::fputc('\n', stdout);
        }
    }
};

void restore_console_level();

int cmd_screenshot(int, char **)
{
    Base64Out out;
    uint16_t width = 0;
    uint16_t height = 0;
    // no log lines in the middle of the picture (the web UI's log still gets them)
    hc::logbuf::set_console_level(ESP_LOG_NONE, ESP_LOG_NONE);
    std::printf("SCREENSHOT-BEGIN\n");
    const esp_err_t err = hc::uidebug::screenshot(
        [](const void *data, size_t size, void *context) {
            static_cast<Base64Out *>(context)->write(static_cast<const uint8_t *>(data), size);
        },
        &out, width, height);
    out.finish();
    std::printf("SCREENSHOT-END %u %u %u\n", width, height, static_cast<unsigned>(out.total));
    std::fflush(stdout);
    restore_console_level();
    return check(err);
}

bool point(char **argv, int16_t &x, int16_t &y)
{
    x = static_cast<int16_t>(std::atoi(argv[0]));
    y = static_cast<int16_t>(std::atoi(argv[1]));
    return x >= 0 && y >= 0;
}

int cmd_tap(int argc, char **argv)
{
    int16_t x = 0;
    int16_t y = 0;
    if (argc < 3 || !point(argv + 1, x, y)) {
        return usage("tap <x> <y> [ms]");
    }
    const int hold = argc > 3 ? std::atoi(argv[3]) : 100;
    hc::uidebug::touch(x, y, true);
    vTaskDelay(pdMS_TO_TICKS(hold));
    hc::uidebug::touch(x, y, false);
    return 0;
}

int cmd_press(int argc, char **argv)
{
    int16_t x = 0;
    int16_t y = 0;
    if (argc != 3 || !point(argv + 1, x, y)) {
        return usage("press <x> <y>");
    }
    hc::uidebug::touch(x, y, true);
    return 0;
}

int cmd_release(int, char **)
{
    hc::uidebug::touch(0, 0, false);
    return 0;
}

int cmd_swipe(int argc, char **argv)
{
    int16_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    if (argc < 5 || !point(argv + 1, x1, y1) || !point(argv + 3, x2, y2)) {
        return usage("swipe <x1> <y1> <x2> <y2> [ms]");
    }
    const int duration = argc > 5 ? std::atoi(argv[5]) : 300;
    constexpr int kSteps = 15;
    for (int i = 0; i <= kSteps; i++) {
        hc::uidebug::touch(static_cast<int16_t>(x1 + (x2 - x1) * i / kSteps),
                           static_cast<int16_t>(y1 + (y2 - y1) * i / kSteps), true);
        vTaskDelay(pdMS_TO_TICKS(duration / kSteps));
    }
    hc::uidebug::touch(x2, y2, false);
    return 0;
}

int cmd_login_reset(int argc, char **argv)
{
    if (argc != 2 || std::strcmp(argv[1], "yes") != 0) {
        return usage("login-reset yes   (removes the web UI login, the web UI is open until a new one exists)");
    }
    return check(hc::webserver::reset_login());
}

int cmd_restart(int, char **)
{
    esp_restart();
    return 0;
}

// Heap per kind of memory: free now and the largest block one allocation can get (KB). Internal
// includes IRAM on the ESP32, which data cannot use; 8-bit is the part that can hold data. No
// "lowest since the start": ESP-IDF sums the low point of every region, so one region that was
// full once reads as 0 although the others had room.
int cmd_mem(int, char **)
{
    struct Row {
        const char *name;
        uint32_t caps;
    };
    static constexpr Row kRows[] = {{"internal", MALLOC_CAP_INTERNAL},
                                    {"internal 8-bit", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT},
                                    {"DMA", MALLOC_CAP_DMA},
                                    {"PSRAM", MALLOC_CAP_SPIRAM}};
    std::printf("%-15s %8s %8s\n", "", "free", "largest");
    for (const auto &row : kRows) {
        multi_heap_info_t info{};
        heap_caps_get_info(&info, row.caps);
        if (info.total_free_bytes + info.total_allocated_bytes == 0) {
            continue;
        }
        std::printf("%-15s %5u KB %5u KB\n", row.name, static_cast<unsigned>(info.total_free_bytes / 1024),
                    static_cast<unsigned>(info.largest_free_block / 1024));
    }
    return 0;
}

// ------------------------------------------------------------------------------ log output

constexpr const char *kSettingsNamespace = "console";
constexpr const char *kKeyLogLevel = "log_level";
constexpr const char *kKeyLogLvgl = "log_lvgl";

struct LevelName {
    const char *name;
    esp_log_level_t level;
};

constexpr LevelName kLevels[] = {
    {"off", ESP_LOG_NONE}, {"errors", ESP_LOG_ERROR}, {"warnings", ESP_LOG_WARN},
    {"on", ESP_LOG_INFO},  {"info", ESP_LOG_INFO},    {"debug", ESP_LOG_DEBUG},
};

esp_log_level_t g_log_level = ESP_LOG_INFO;
// the periodic reports of the test screen and its touch messages (tag "lvgl", LV_LOG_USER) are
// only wanted while looking at them: `log lvgl`
bool g_log_lvgl = false;

const char *level_name(esp_log_level_t level)
{
    for (const auto &entry : kLevels) {
        if (entry.level == level) {
            return entry.name;
        }
    }
    return "?";
}

// The level for every tag, stored so the console stays quiet after a restart as well. The boot
// messages before the console starts are not affected.
void apply_log_level(esp_log_level_t level, bool store)
{
    g_log_level = level;
    // warnings and errors of LVGL stay visible; the web UI's log keeps its own level
    hc::logbuf::set_console_level(level, g_log_lvgl || level < ESP_LOG_INFO ? level : ESP_LOG_WARN);
    if (store) {
        hc::platform::Settings settings(kSettingsNamespace, hc::platform::Settings::Mode::ReadWrite);
        if (settings.set_u8(kKeyLogLevel, static_cast<uint8_t>(level)) == ESP_OK) {
            settings.commit();
        }
    }
}

void restore_console_level()
{
    apply_log_level(g_log_level, false);
}

int cmd_log(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "lvgl") == 0) {
        g_log_lvgl = !g_log_lvgl;
        hc::platform::Settings settings(kSettingsNamespace, hc::platform::Settings::Mode::ReadWrite);
        if (settings.set_u8(kKeyLogLvgl, g_log_lvgl ? 1 : 0) == ESP_OK) {
            settings.commit();
        }
        apply_log_level(g_log_level, false);
        std::printf("lvgl messages (test screen report, touches) %s\n", g_log_lvgl ? "on" : "off");
        return 0;
    }
    if (argc == 1) {
        // toggle between quiet and the normal output
        apply_log_level(g_log_level == ESP_LOG_NONE ? ESP_LOG_INFO : ESP_LOG_NONE, true);
        std::printf("log output %s\n", level_name(g_log_level));
        return 0;
    }
    if (argc == 2) {
        for (const auto &entry : kLevels) {
            if (std::strcmp(argv[1], entry.name) == 0) {
                apply_log_level(entry.level, true);
                std::printf("log output %s\n", level_name(g_log_level));
                return 0;
            }
        }
    }
    return usage("log [off|errors|warnings|on|debug|lvgl]   (without an argument: toggle off/on; lvgl toggles the test screen messages)");
}

void restore_log_level()
{
    hc::platform::Settings settings(kSettingsNamespace, hc::platform::Settings::Mode::Read);
    uint8_t value = 0;
    if (settings.ok() && settings.get_u8(kKeyLogLvgl, value) == ESP_OK) {
        g_log_lvgl = value != 0;
    }
    value = 0;
    if (settings.ok() && settings.get_u8(kKeyLogLevel, value) == ESP_OK && value <= ESP_LOG_VERBOSE) {
        apply_log_level(static_cast<esp_log_level_t>(value), false);
    } else {
        apply_log_level(g_log_level, false);
    }
}

// Orientation sensor measurement while a board is added (docs/adding-a-board.md): the result goes
// into the board info, nothing is stored on the device.
int cmd_orient(int argc, char **argv)
{
    if (argc == 3 && std::strcmp(argv[1], "capture") == 0) {
        using hc::orientation::Pose;
        struct PoseName {
            const char *name;
            Pose pose;
        };
        static const PoseName kPoses[] = {{"0", Pose::Up0},     {"90", Pose::Up90}, {"180", Pose::Up180},
                                          {"270", Pose::Up270}, {"flat", Pose::Flat}};
        if (std::strcmp(argv[2], "clear") == 0) {
            return check(hc::orientation::clear_captures());
        }
        const PoseName *chosen = nullptr;
        for (const auto &entry : kPoses) {
            if (std::strcmp(argv[2], entry.name) == 0) {
                chosen = &entry;
            }
        }
        if (chosen == nullptr) {
            return usage("orient capture 0|90|180|270|flat|clear");
        }
        hc::orientation::Measurement m;
        if (check(hc::orientation::capture(chosen->pose, m)) != 0) {
            return 1;
        }
        std::printf("%s captured, %.2f g\n", chosen->name, static_cast<double>(m.length));
        if (!m.complete) {
            std::printf("still to capture:");
            for (const auto &entry : kPoses) {
                if ((m.captured & (1u << static_cast<int>(entry.pose))) == 0) {
                    std::printf(" %s", entry.name);
                }
            }
            std::printf("\n(0 = panel exactly upright in the base orientation, 90 = turned 90 degrees CLOCKWISE, 180, 270; "
                        "flat = lying on the table, screen up; hold each one still)\n");
            return 0;
        }
        std::printf("the module reads %.2f g for 1 g, 0 and 90 are %.1f degrees apart, the module is tilted %.1f degrees "
                    "against the panel, the offsets of the two pairs differ by %.2f g\n",
                    static_cast<double>(m.gain), static_cast<double>(m.angle_deg), static_cast<double>(m.tilt_deg),
                    static_cast<double>(m.offset_mismatch));
        std::printf("board info:\n    .imu_axes = {%.4ff, %.4ff, %.4ff, %.4ff, %.4ff, %.4ff},\n"
                    "    .imu_offset = {%.4ff, %.4ff, %.4ff},\n",
                    static_cast<double>(m.axes[0]), static_cast<double>(m.axes[1]), static_cast<double>(m.axes[2]),
                    static_cast<double>(m.axes[3]), static_cast<double>(m.axes[4]), static_cast<double>(m.axes[5]),
                    static_cast<double>(m.offset[0]), static_cast<double>(m.offset[1]), static_cast<double>(m.offset[2]));
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "raw") == 0) {
        hc::orientation::Raw r;
        if (check(hc::orientation::read_raw(r)) != 0) {
            return 1;
        }
        std::printf("WHO_AM_I 0x%02x (0x68), PWR_MGMT_1 0x%02x (0x01), CONFIG 0x%02x (0x03), ACCEL_CONFIG 0x%02x (0x00)\n",
                    r.who_am_i, r.power_management, r.config, r.accel_config);
        std::printf("accel raw %d %d %d (16384 = 1 g), gyro raw %d %d %d (about 0 at rest)\n", r.accel[0], r.accel[1],
                    r.accel[2], r.gyro[0], r.gyro[1], r.gyro[2]);
        return 0;
    }
    if (argc == 1) {
        hc::orientation::Gravity g;
        if (!hc::orientation::gravity(g)) {
            std::printf("no reading\n");
            return 1;
        }
        const double length = std::sqrt(static_cast<double>(g.x * g.x + g.y * g.y + g.z * g.z));
        std::printf("gravity x %.2f y %.2f z %.2f g (length %.2f), %s\n", static_cast<double>(g.x),
                    static_cast<double>(g.y), static_cast<double>(g.z), length, g.at_rest ? "at rest" : "moving");
        const double corrected = std::sqrt(static_cast<double>(g.cx * g.cx + g.cy * g.cy + g.cz * g.cz));
        std::printf("without the offset of the board x %.2f y %.2f z %.2f g (length %.2f)\n", static_cast<double>(g.cx),
                    static_cast<double>(g.cy), static_cast<double>(g.cz), corrected);
        std::printf("motion: the reading changed by %.3f g between two samples in the last second (rest: below 0.05)\n",
                    static_cast<double>(g.motion_g));
        std::printf("reads since boot: %lu failed, %lu needed a retry\n",
                    static_cast<unsigned long>(hc::orientation::failed_reads()),
                    static_cast<unsigned long>(hc::orientation::retried_reads()));
        return 0;
    }
    return usage("orient [raw|capture 0|90|180|270|flat|clear]");
}

int print_ldr()
{
    float mv = 0.0f;
    float ohms = 0.0f;
    float lux = 0.0f;
    float default_lux = 0.0f;
    if (!hc::light_sensor::ldr::reading(mv, ohms) || !hc::light_sensor::ldr::lux(lux) ||
        !hc::light_sensor::ldr::default_lux(default_lux)) {
        std::printf("LDR: no reading yet\n");
        return 1;
    }
    hc::light_sensor::ldr::Point points[hc::light_sensor::ldr::kMaxPoints];
    const size_t count = hc::light_sensor::ldr::points(points, hc::light_sensor::ldr::kMaxPoints);
    std::printf("LDR %.0f mV, %.0f Ohm -> %.1f lx (%u points; default curve %.1f lx)\n", static_cast<double>(mv),
                static_cast<double>(ohms), static_cast<double>(lux), static_cast<unsigned>(count),
                static_cast<double>(default_lux));
    return 0;
}

// one line per second: the numbers to put next to a reference lux meter
int watch_ldr(int seconds)
{
    for (int i = 0; i < seconds; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (print_ldr() != 0) {
            return 1;
        }
    }
    return 0;
}

int list_points()
{
    hc::light_sensor::ldr::Point points[hc::light_sensor::ldr::kMaxPoints];
    const size_t count = hc::light_sensor::ldr::points(points, hc::light_sensor::ldr::kMaxPoints);
    for (size_t i = 0; i < count; i++) {
        std::printf("%8.0f Ohm = %8.1f lx\n", static_cast<double>(points[i].ohms), static_cast<double>(points[i].lux));
    }
    std::printf("%u points\n", static_cast<unsigned>(count));
    return 0;
}

int print_light()
{
    if (hc::light_sensor::ldr::active()) {
        return print_ldr();
    }
    float raw = 0.0f;
    float lux = 0.0f;
    if (!hc::light_sensor::raw_lux(raw) || !hc::light_sensor::lux(lux)) {
        std::printf("no light sensor, or it does not answer\n");
        return 1;
    }
    bool calibrated = false;
    const float scale = hc::light_sensor::scale(&calibrated);
    const auto &info = hc::board::info();
    const float level = hc::board::backlight_level();
    std::printf("measured %.1f lx, scale %.3f (%s) -> %.1f lx\n", static_cast<double>(raw), static_cast<double>(scale),
                calibrated ? "this device, from `light calibrate`" : "board default", static_cast<double>(lux));
    std::printf("backlight %.0f %%, the screen's own light at full: %.1f lx -> room %.1f lx\n",
                static_cast<double>(level * 100.0f), static_cast<double>(info.light_sensor_backlight_lux),
                static_cast<double>(lux - info.light_sensor_backlight_lux * level));
    return 0;
}

// lux after the sensor had time for two measurements (1 s average), before the board's scale
float settled_raw_lux()
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    float value = 0.0f;
    if (hc::light_sensor::ldr::active()) {
        hc::light_sensor::ldr::lux(value); // the points are the scale
    } else {
        hc::light_sensor::raw_lux(value);
    }
    return value;
}

int cmd_light(int argc, char **argv)
{
    if (argc == 1) {
        return print_light();
    }
    if (argc == 3 && std::strcmp(argv[1], "calibrate") == 0) {
        const float reference = std::strtof(argv[2], nullptr);
        if (check(hc::light_sensor::calibrate(reference)) != 0) {
            return 1;
        }
        if (!hc::light_sensor::ldr::active()) { // the LDR keeps a table of points, see `light points`
            std::printf("board info: .light_sensor_scale = %.3ff\n", static_cast<double>(hc::light_sensor::scale()));
        }
        return print_light();
    }
    if (argc == 2 && std::strcmp(argv[1], "reset") == 0) {
        return check(hc::light_sensor::reset_calibration());
    }
    if (hc::light_sensor::ldr::active() && argc >= 2 && std::strcmp(argv[1], "watch") == 0) {
        return watch_ldr(argc == 3 ? std::atoi(argv[2]) : 10);
    }
    if (hc::light_sensor::ldr::active() && argc == 4 && std::strcmp(argv[1], "point") == 0) {
        return check(hc::light_sensor::ldr::add_point(std::strtof(argv[2], nullptr), std::strtof(argv[3], nullptr)));
    }
    if (hc::light_sensor::ldr::active() && argc == 2 && std::strcmp(argv[1], "points") == 0) {
        return list_points();
    }
    if (argc == 2 && std::strcmp(argv[1], "backlight") == 0) {
        // in the dark, with Automatic off: what the sensor sees of the screen itself
        const uint16_t max = hc::board::backlight_max_duty();
        const auto before = static_cast<uint16_t>(hc::board::backlight_level() * static_cast<float>(max) + 0.5f);
        hc::board::set_backlight_duty(0);
        const float dark = settled_raw_lux();
        hc::board::set_backlight_duty(max);
        const float bright = settled_raw_lux();
        hc::board::set_backlight_duty(before);
        const float own = (bright - dark) * hc::light_sensor::scale(); // 1 for the LDR
        std::printf("backlight off %.1f lx, full %.1f lx (measured)\n", static_cast<double>(dark),
                    static_cast<double>(bright));
        std::printf("board info: .light_sensor_backlight_lux = %.1ff\n", static_cast<double>(own < 0.0f ? 0.0f : own));
        return 0;
    }
    return usage("light [calibrate <lux of a reference meter>|reset|backlight|watch [seconds]|points|point <ohms> <lux>]");
}

// The values of a dashboard page from Home Assistant, before the dashboard screen exists: the
// console takes the place of the UI (hass::subscribe_page, hass::take_event).
std::shared_ptr<const hc::core::Layout> g_hass_layout;
uint32_t g_hass_generation = 0;

void print_number(const char *name, float value)
{
    if (!std::isnan(value)) {
        std::printf(" %s=%g", name, static_cast<double>(value));
    }
}

void print_list(const char *name, const hc::memory::Vector<hc::memory::String> &list)
{
    if (list.empty()) {
        return;
    }
    std::printf(" %s=[", name);
    for (size_t i = 0; i < list.size(); i++) {
        std::printf("%s%s", i > 0 ? "|" : "", list[i].c_str());
    }
    std::printf("]");
}

void print_update(const hc::core::EntityUpdate &update)
{
    using hc::core::Field;
    const hc::core::EntityState &v = update.values;
    const char *id = g_hass_layout && update.entity < g_hass_layout->entities.size()
                         ? g_hass_layout->entities[update.entity].c_str()
                         : "?";
    std::printf("  %s: '%s'", id, v.state.c_str());
    const auto text = [&](Field field, const char *name, const hc::memory::String &value) {
        if ((update.fields & hc::core::bit(field)) != 0 && !value.empty()) {
            std::printf(" %s='%s'", name, value.c_str());
        }
    };
    text(Field::Rounded, "rounded", v.rounded);
    text(Field::Unit, "unit", v.unit);
    text(Field::Name, "name", v.name);
    text(Field::Icon, "icon", v.icon);
    text(Field::DeviceClass, "class", v.device_class);
    if (v.brightness >= 0) {
        std::printf(" brightness=%d", v.brightness);
    }
    print_list("modes", v.color_modes);
    text(Field::ColorMode, "mode", v.color_mode);
    text(Field::Effect, "effect", v.effect);
    print_list("effects", v.effect_list);
    print_number("kelvin", v.color_temp);
    print_number("hue", v.hue);
    print_number("sat", v.saturation);
    print_number("target", v.target);
    print_number("current", v.current);
    text(Field::HvacAction, "action", v.hvac_action);
    print_number("min", v.min_temp);
    print_number("max", v.max_temp);
    print_number("step", v.temp_step);
    print_list("hvac", v.hvac_modes);
    text(Field::PresetMode, "preset", v.preset_mode);
    print_list("presets", v.preset_modes);
    std::printf("\n");
}

#if CONFIG_HC_AUDIO
// The speaker without the screen: a tone or the click at a volume (the stored one comes back
// with the next change in Settings, Sound)
int cmd_sound(int argc, char **argv)
{
    if (!hc::audio::running()) {
        std::printf("no audio output\n");
        return 1;
    }
    if (argc == 1) {
        std::printf("volume %u %%, alarm %s, audio task stack: %u bytes never used, internal RAM free %u KB\n",
                    hc::audio::volume(), hc::audio::alarm_playing() ? "playing" : "off",
                    static_cast<unsigned>(hc::audio::stack_unused()),
                    static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        return 0;
    }
    if (std::strcmp(argv[1], "alarm") == 0) {
        hc::audio::play_alarm(argc > 2 ? argv[2] : nullptr, true);
        return 0;
    }
    if (std::strcmp(argv[1], "stop") == 0) {
        hc::audio::stop_alarm();
        return 0;
    }
#if CONFIG_HC_AUDIO_ALARM
    if (std::strcmp(argv[1], "ring") == 0) {
        // the alarm of Settings with its screen, as if its time had come
        hc::ui::ring_alarm();
        return 0;
    }
#endif
#if CONFIG_HC_AUDIO_MP3
    if (std::strcmp(argv[1], "play") == 0 && argc == 3) {
        hc::audio::play_file(argv[2]);
        return 0;
    }
#endif
    if (std::strcmp(argv[1], "volume") == 0 && argc == 3) {
        hc::audio::set_volume(static_cast<uint8_t>(std::atoi(argv[2])));
        return 0;
    }
    if (std::strcmp(argv[1], "tone") == 0) {
        const int hz = argc > 2 ? std::atoi(argv[2]) : 1000;
        const int ms = argc > 3 ? std::atoi(argv[3]) : 500;
        hc::audio::play_tone(static_cast<uint16_t>(hz), static_cast<uint16_t>(ms));
        return 0;
    }
#if CONFIG_HC_AUDIO_TOUCH_SOUND
    if (std::strcmp(argv[1], "click") == 0) {
        hc::audio::play_click();
        return 0;
    }
    if (std::strcmp(argv[1], "tick") == 0) {
        hc::audio::play_tick();
        return 0;
    }
#endif
    return usage("sound [volume <percent>|tone [hz] [ms]|click|tick|play <file>|alarm [file]|stop|ring]");
}
#endif

#if CONFIG_HC_VIBRATION
// The motor without the screen: a pulse of a length (for tuning the click), the alarm pattern
int cmd_vibrate(int argc, char **argv)
{
    if (!hc::vibration::running()) {
        std::printf("no vibration motor\n");
        return 1;
    }
    if (argc == 1 || std::strcmp(argv[1], "click") == 0) {
        hc::vibration::click();
        return 0;
    }
    if (std::strcmp(argv[1], "alarm") == 0) {
        hc::vibration::start_alarm();
        return 0;
    }
    if (std::strcmp(argv[1], "stop") == 0) {
        hc::vibration::stop_alarm();
        return 0;
    }
    if (std::strcmp(argv[1], "status") == 0) {
        std::printf("motor %s\n", hc::vibration::motor_on() ? "on" : "off");
        return 0;
    }
    const int ms = std::atoi(argv[1]);
    if (ms <= 0) {
        return usage("vibrate [click|<ms>|alarm|stop|status]");
    }
    hc::vibration::pulse(static_cast<uint16_t>(ms));
    return 0;
}
#endif

// the samples of the touch controller (hc::touch_input), for a board whose touch misbehaves
int cmd_touch(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "log") == 0) {
        hc::touch_input::set_trace(!hc::touch_input::trace());
        std::printf("touch samples %s\n", hc::touch_input::trace() ? "on" : "off");
        return 0;
    }
    return usage("touch log   (toggles printing the controller's samples)");
}

int print_hass_events()
{
    hc::hass::HassEvent event;
    int count = 0;
    while (hc::hass::take_event(event)) {
        count++;
        if (const auto *values = std::get_if<hc::hass::PageValues>(&event)) {
            std::printf("page values, generation %lu%s:\n", static_cast<unsigned long>(values->generation),
                        values->generation != g_hass_generation ? " (old)" : "");
            for (const auto &update : values->updates) {
                print_update(update);
            }
        } else if (const auto *failed = std::get_if<hc::hass::ServiceFailed>(&event)) {
            std::printf("service call failed: %s\n", failed->message.c_str());
        } else if (const auto *history = std::get_if<hc::core::History>(&event)) {
            std::printf("history of entity %u: %u points%s%s\n", history->entity,
                        static_cast<unsigned>(history->points.size()), history->error.empty() ? "" : ", ",
                        history->error.c_str());
        } else if (const auto *image = std::get_if<hc::core::Image>(&event)) {
            std::printf("image of entity %u: %ux%u%s%s\n", image->entity, image->width, image->height,
                        image->error.empty() ? "" : ", ", image->error.c_str());
        }
    }
    if (count == 0) {
        std::printf("no new values\n");
    }
    return 0;
}

int cmd_hass(int argc, char **argv)
{
    if (argc == 3 && std::strcmp(argv[1], "page") == 0) {
        if (std::strcmp(argv[2], "off") == 0) {
            g_hass_layout.reset();
            hc::hass::subscribe_page(nullptr, hc::core::kNone, hc::core::kNone, ++g_hass_generation);
            return 0;
        }
        g_hass_layout = hc::dashboard::current();
        const long page = std::strtol(argv[2], nullptr, 10);
        if (!g_hass_layout || page < 0 || static_cast<size_t>(page) >= g_hass_layout->pages.size()) {
            std::printf("no such page (the dashboard has %u)\n",
                        g_hass_layout ? static_cast<unsigned>(g_hass_layout->pages.size()) : 0u);
            return 1;
        }
        // the panel of the first tile of the page, so its fields come along
        const auto &tiles = g_hass_layout->pages[static_cast<size_t>(page)].tiles;
        const hc::core::TileIndex panel = argc == 3 && !tiles.empty() ? tiles.front() : hc::core::kNone;
        hc::hass::subscribe_page(g_hass_layout, static_cast<hc::core::PageIndex>(page), panel, ++g_hass_generation);
        std::printf("page %ld subscribed, generation %lu; `hass values` prints what arrived\n", page,
                    static_cast<unsigned long>(g_hass_generation));
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "values") == 0) {
        return print_hass_events();
    }
    if ((argc == 3 && std::strcmp(argv[1], "toggle") == 0) || (argc == 4 && std::strcmp(argv[1], "level") == 0)) {
        hc::core::Intent intent;
        intent.kind = argc == 3 ? hc::core::IntentKind::Toggle : hc::core::IntentKind::SetLevel;
        if (argc == 4) {
            intent.value = std::strtof(argv[3], nullptr);
        }
        hc::hass::call_service(intent, argv[2]);
        return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "streamscale") == 0) {
        // for measurements: the decoder's scale of a stream that plays full screen
        const bool automatic = std::strcmp(argv[2], "auto") == 0;
        const long shift = automatic ? -1 : std::strtol(argv[2], nullptr, 10);
        if (!automatic && (shift < 0 || shift > 3)) {
            return usage("hass streamscale auto|0|1|2|3 (1:1 to 1:8)");
        }
        hc::hass::set_stream_scale(static_cast<int>(shift));
        std::printf("stream scale %s, from the next frame on\n", automatic ? "auto" : argv[2]);
        return 0;
    }
    return usage("hass page <n>|off, hass values, hass toggle <entity>, hass level <entity> <percent>, "
                 "hass streamscale auto|0..3");
}

int cmd_setup(int, char **)
{
    hc::ui::show_setup();
    return 0;
}

int cmd_factory_reset(int argc, char **argv)
{
    if (argc != 2 || std::strcmp(argv[1], "yes") != 0) {
        return usage("factory-reset yes   (erases every setting, then restarts)");
    }
    if (check(hc::platform::erase_all_settings()) != 0) {
        return 1;
    }
    esp_restart();
    return 0;
}

esp_err_t add(const char *name, const char *help, const char *hint, esp_console_cmd_func_t func)
{
    esp_console_cmd_t command{};
    command.command = name;
    command.help = help;
    command.hint = hint;
    command.func = func;
    return esp_console_cmd_register(&command);
}

TaskHandle_t g_task = nullptr;

constexpr const char *kPrompt = "hassctrl> ";
// `screenshot` renders the screen with LVGL on this task (the LVGL task itself has 7 KB)
constexpr uint32_t kStackSize = 10240;

// The console's port with its driver, as esp_console's REPL sets it up: Enter sends CR, a line
// ends in CRLF. A UART, or the USB-Serial/JTAG of boards without a USB-UART bridge (JC3248W535).
void init_port()
{
    fflush(stdout);
    fsync(fileno(stdout));
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
    usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
    usb_serial_jtag_vfs_use_driver();
#else
    uart_vfs_dev_port_set_rx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_CR);
    uart_vfs_dev_port_set_tx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM, ESP_LINE_ENDINGS_CRLF);
    uart_config_t config{};
    config.baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.source_clk = UART_SCLK_DEFAULT;
    constexpr auto kUart = static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM);
    ESP_ERROR_CHECK(uart_driver_install(kUart, 256, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(kUart, &config));
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
#endif
    setvbuf(stdin, nullptr, _IONBF, 0);
#if CONFIG_LIBC_PICOLIBC
    tls_stdin = stdin;
    tls_stdout = stdout;
#endif
}

void init_console()
{
    esp_console_config_t config = ESP_CONSOLE_CONFIG_DEFAULT();
    config.max_cmdline_length = 256;
    config.max_cmdline_args = 12;
    ESP_ERROR_CHECK(esp_console_init(&config));
    linenoiseSetMultiLine(1);
    linenoiseSetCompletionCallback(&esp_console_get_completion);
    linenoiseSetHintsCallback(reinterpret_cast<linenoiseHintsCallback *>(&esp_console_get_hint));
    linenoiseHistorySetMaxLen(32);
    linenoiseSetMaxLineLen(config.max_cmdline_length);
    linenoiseAllowEmpty(false);
    // a terminal without escape sequences gets plain lines
    if (linenoiseProbe() != 0) {
        linenoiseSetDumbMode(1);
    }
}

void register_commands()
{
    esp_console_register_help_command();
    add("wifi", "Wi-Fi: status, saved networks, scan, reconnect", "[status|list|add|connect|remove|up|down|policy|scan|results|reconnect]",
        cmd_wifi);
    add("hotspot", "turn the hotspot on or off, or give it a new password", "on|off|password", cmd_hotspot);
    add("c6", "Wi-Fi coprocessor: firmware version, update from a URL", "[version|update <http url>]", cmd_c6);
    add("fs", "file system status", nullptr, cmd_fs);
    add("mem", "free memory: internal, DMA, PSRAM", nullptr, cmd_mem);
    add("restart", "restart the device", nullptr, cmd_restart);
    add("reboot", "restart the device", nullptr, cmd_restart);
    add("login-reset", "remove the web UI login", "yes", cmd_login_reset);
    add("screenshot", "screen as run-length encoded RGB565 in base64 (tools/screenshot.py)", nullptr, cmd_screenshot);
    add("tap", "tap the screen", "<x> <y> [ms]", cmd_tap);
    add("press", "press the screen", "<x> <y>", cmd_press);
    add("release", "release the screen", nullptr, cmd_release);
    add("swipe", "swipe across the screen", "<x1> <y1> <x2> <y2> [ms]", cmd_swipe);
    add("log", "log output on the serial port: toggle, or set a level", "[off|errors|warnings|on|debug]", cmd_log);
    add("factory-reset", "erase every setting and restart", "yes", cmd_factory_reset);
    add("hass", "Home Assistant: subscribe a dashboard page, print its values, call a service",
        "page <n>|off, values, toggle <entity>, level <entity> <percent>", cmd_hass);
    add("setup", "show the first setup screens (they come by themselves while no network is saved)", nullptr,
        cmd_setup);
    add("orient", "orientation sensor: gravity, board measurement (developer)", "[raw|capture 0|90|180|270|flat|clear]", cmd_orient);
    add("touch", "touch controller: print its samples", "log", cmd_touch);
    add("light", "light sensor: values, calibration while adding a board", "[calibrate <lux>|reset|backlight|watch [s]|points]",
        cmd_light);
#if CONFIG_HC_AUDIO
    add("sound", "speaker: volume, test tone, touch sound", "[volume <percent>|tone [hz] [ms]|click|tick|play <file>|alarm [file]|stop|ring]", cmd_sound);
#endif
#if CONFIG_HC_VIBRATION
    add("vibrate", "vibration motor: the click, a pulse, the alarm pattern", "[click|<ms>|alarm|stop|status]", cmd_vibrate);
#endif
}

// The console's own task: created at the start, so its stack is taken while the internal RAM is
// in one piece. The probe of the terminal runs here, not in app_main.
void console_task(void *)
{
    init_port();
    init_console();
    restore_log_level();
    register_commands();
    // the commands use the network and the other components: wait until they run
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    std::printf("\r\nType 'help' to get the list of commands.\r\n");
    for (;;) {
        char *line = linenoise(kPrompt);
        if (line == nullptr) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        linenoiseHistoryAdd(line);
        int result = 0;
        const esp_err_t err = esp_console_run(line, &result);
        if (err == ESP_ERR_NOT_FOUND) {
            std::printf("Unrecognized command\n");
        } else if (err == ESP_OK && result != 0) {
            std::printf("Command returned non-zero error code: 0x%x (%s)\n", result, esp_err_to_name(result));
        } else if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
            std::printf("Internal error: %s\n", esp_err_to_name(err));
        }
        linenoiseFree(line);
    }
}

} // namespace

esp_err_t start()
{
    return xTaskCreate(console_task, "console", kStackSize, nullptr, 2, &g_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void ready()
{
    if (g_task != nullptr) {
        xTaskNotifyGive(g_task);
    }
}

#else

esp_err_t start()
{
    return ESP_ERR_NOT_SUPPORTED;
}

void ready()
{
}

#endif

} // namespace hc::devconsole
