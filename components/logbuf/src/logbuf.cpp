#include "hc/logbuf/logbuf.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hc/platform/settings.hpp"

namespace hc::logbuf {

namespace {

constexpr size_t kCapacity = 16 * 1024;
// without PSRAM (it failed), a small one in internal RAM
constexpr size_t kFallbackCapacity = 2 * 1024;
// a message as esp_log formats it, cut there
constexpr size_t kLineMax = 200;
// times before this are uptime, the clock is not set yet (2020-09-13)
constexpr time_t kClockSet = 1600000000;

constexpr const char *kSettingsNamespace = "logbuf";
constexpr const char *kKeyDebug = "debug";

char *g_buffer = nullptr;
size_t g_capacity = 0;
// where the next line goes and how many bytes before it are lines
size_t g_head = 0;
size_t g_used = 0;
SemaphoreHandle_t g_mutex = nullptr;
StaticSemaphore_t g_mutex_storage;
vprintf_like_t g_previous = nullptr;

bool g_debug = false;
std::atomic<uint8_t> g_capture{ESP_LOG_WARN};
std::atomic<uint8_t> g_console{CONFIG_LOG_DEFAULT_LEVEL};
std::atomic<uint8_t> g_console_lvgl{CONFIG_LOG_DEFAULT_LEVEL};

// The level of an esp_log line ("E (1234) tag: ..."), ESP_LOG_NONE for other output.
esp_log_level_t level_of(const char *line, size_t length)
{
    if (length < 4 || line[1] != ' ' || line[2] != '(') {
        return ESP_LOG_NONE;
    }
    switch (line[0]) {
    case 'E':
        return ESP_LOG_ERROR;
    case 'W':
        return ESP_LOG_WARN;
    case 'I':
        return ESP_LOG_INFO;
    case 'D':
        return ESP_LOG_DEBUG;
    case 'V':
        return ESP_LOG_VERBOSE;
    default:
        return ESP_LOG_NONE;
    }
}

bool is_lvgl(const char *line)
{
    const char *tag = std::strstr(line, ") ");
    return tag != nullptr && std::strncmp(tag + 2, "lvgl:", 5) == 0;
}

// removes the oldest line
void drop_oldest()
{
    size_t tail = (g_head + g_capacity - g_used) % g_capacity;
    while (g_used > 0) {
        const char c = g_buffer[tail];
        tail = (tail + 1) % g_capacity;
        g_used--;
        if (c == '\n') {
            return;
        }
    }
}

void put(const char *data, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        g_buffer[g_head] = data[i];
        g_head = (g_head + 1) % g_capacity;
    }
}

// One line, the end of line and line breaks inside it replaced; the mutex is held.
void store_locked(char *line, size_t length)
{
    while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
        length--;
    }
    for (size_t i = 0; i < length; i++) {
        if (line[i] == '\n' || line[i] == '\r') {
            line[i] = ' ';
        }
    }
    const time_t now = std::time(nullptr);
    char stamp[16];
    const int stamp_length =
        std::snprintf(stamp, sizeof(stamp), "%lu ", now >= kClockSet ? static_cast<unsigned long>(now) : 0UL);
    const size_t total = static_cast<size_t>(stamp_length) + length + 1;
    if (total > g_capacity) {
        return;
    }
    while (g_used + total > g_capacity) {
        drop_oldest();
    }
    put(stamp, static_cast<size_t>(stamp_length));
    put(line, length);
    put("\n", 1);
    g_used += total;
}

// A message written in pieces: the Wi-Fi driver writes "I (1234) wifi:" and the text in separate
// calls. The pieces without a header that follow from the same task until the end of line belong
// to it: printed if the header was, kept with it. One such message at a time (the mutex is held
// while it is used).
struct Pending {
    TaskHandle_t task = nullptr;
    bool print = false;
    bool keep = false;
    size_t length = 0;
    char text[kLineMax + 1];
};
Pending g_pending;

void finish_pending_locked()
{
    if (g_pending.keep) {
        store_locked(g_pending.text, g_pending.length);
    }
    g_pending.task = nullptr;
}

void append_pending(const char *text, size_t length)
{
    const size_t room = kLineMax - g_pending.length;
    const size_t n = length < room ? length : room;
    std::memcpy(g_pending.text + g_pending.length, text, n);
    g_pending.length += n;
}

// Every esp_log message passes here. Formatting it once serves both: the serial output prints
// the formatted text (less stack than the previous vprintf), the buffer keeps it.
int output(const char *format, va_list args)
{
    char line[kLineMax + 1];
    va_list copy;
    va_copy(copy, args);
    const int n = std::vsnprintf(line, sizeof(line), format, copy);
    va_end(copy);
    if (n < 0) {
        return n;
    }
    const bool whole = static_cast<size_t>(n) < sizeof(line);
    const size_t length = whole ? static_cast<size_t>(n) : sizeof(line) - 1;
    const esp_log_level_t level = level_of(line, length);
    const uint8_t console = is_lvgl(line) ? g_console_lvgl.load() : g_console.load();
    bool print = level == ESP_LOG_NONE || level <= console;
    const bool keep = level != ESP_LOG_NONE && level <= g_capture.load();
    // a line cut short counts as ended
    const bool ended = !whole || (length > 0 && line[length - 1] == '\n');

    if (g_buffer != nullptr && !xPortInIsrContext() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        const TaskHandle_t task = xTaskGetCurrentTaskHandle();
        xSemaphoreTake(g_mutex, portMAX_DELAY);
        if (level == ESP_LOG_NONE && g_pending.task == task) {
            print = g_pending.print;
            if (g_pending.keep) {
                append_pending(line, length);
            }
            if (ended) {
                finish_pending_locked();
            }
        } else if (level != ESP_LOG_NONE) {
            if (g_pending.task == task) {
                finish_pending_locked();
            }
            if (!ended && g_pending.task == nullptr) {
                g_pending.task = task;
                g_pending.print = print;
                g_pending.keep = keep;
                g_pending.length = 0;
                append_pending(line, length);
            } else if (keep) {
                store_locked(line, length);
            }
        }
        xSemaphoreGive(g_mutex);
    }
    if (print) {
        if (whole) {
            std::fputs(line, stdout);
        } else {
            g_previous(format, args);
        }
    }
    return n;
}

esp_log_level_t more_detailed(uint8_t a, uint8_t b)
{
    return static_cast<esp_log_level_t>(a > b ? a : b);
}

// esp_log makes the messages of the more detailed level of the console and the buffer
void apply()
{
    g_capture = g_buffer == nullptr ? ESP_LOG_NONE : (g_debug ? ESP_LOG_INFO : ESP_LOG_WARN);
    esp_log_level_set("*", more_detailed(g_console, g_capture));
    // after the wildcard, which resets every tag
    esp_log_level_set("lvgl", more_detailed(g_console_lvgl, g_capture));
}

} // namespace

void start()
{
    g_mutex = xSemaphoreCreateMutexStatic(&g_mutex_storage);
    g_buffer = static_cast<char *>(heap_caps_malloc(kCapacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    g_capacity = kCapacity;
    if (g_buffer == nullptr) {
        g_buffer = static_cast<char *>(heap_caps_malloc(kFallbackCapacity, MALLOC_CAP_8BIT));
        g_capacity = g_buffer == nullptr ? 0 : kFallbackCapacity;
    }
    g_previous = esp_log_set_vprintf(output);
    apply();
}

void restore()
{
    hc::platform::Settings settings(kSettingsNamespace, hc::platform::Settings::Mode::Read);
    uint8_t value = 0;
    g_debug = settings.ok() && settings.get_u8(kKeyDebug, value) == ESP_OK && value != 0;
    apply();
}

bool debug()
{
    return g_debug;
}

void set_debug(bool on)
{
    g_debug = on;
    apply();
    hc::platform::Settings settings(kSettingsNamespace, hc::platform::Settings::Mode::ReadWrite);
    if (settings.set_u8(kKeyDebug, on ? 1 : 0) == ESP_OK) {
        settings.commit();
    }
}

void set_console_level(esp_log_level_t level, esp_log_level_t lvgl_level)
{
    g_console = level;
    g_console_lvgl = lvgl_level;
    apply();
}

size_t capacity()
{
    return g_capacity;
}

size_t copy(char *out, size_t size)
{
    if (g_buffer == nullptr || size == 0) {
        return 0;
    }
    xSemaphoreTake(g_mutex, portMAX_DELAY);
    size_t length = g_used;
    size_t tail = (g_head + g_capacity - g_used) % g_capacity;
    // the newest whole lines when `out` is the smaller one (the last byte is a line end, so this
    // stops at the latest there)
    bool skip = length > size;
    while (skip) {
        const char c = g_buffer[tail];
        tail = (tail + 1) % g_capacity;
        length--;
        skip = c != '\n' || length > size;
    }
    const size_t first = length < g_capacity - tail ? length : g_capacity - tail;
    std::memcpy(out, g_buffer + tail, first);
    std::memcpy(out + first, g_buffer, length - first);
    xSemaphoreGive(g_mutex);
    return length;
}

} // namespace hc::logbuf
