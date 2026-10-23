#include "hc/sdcard/sdcard.hpp"

#include <cstdio>
#include <cstring>

#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

namespace hc::sdcard {

namespace {

constexpr const char *kTag = "sdcard";
// High speed when the card takes it (sdmmc_card_init falls back to 20 MHz otherwise)
constexpr int kMaxClockKhz = SDMMC_FREQ_HIGHSPEED;
// the FAT's open files (each with its sector cache in PSRAM, docs/architecture.md section 9.3)
constexpr int kMaxFiles = 4;
// the cluster size of format(): large clusters for sound and recording files
constexpr size_t kAllocationUnit = 32 * 1024;
constexpr uint32_t kStatusWaitMs = 20;

StaticSemaphore_t g_lock_buffer;
SemaphoreHandle_t g_lock = nullptr;
sd_pwr_ctrl_handle_t g_power = nullptr;
sdmmc_slot_config_t g_slot{};
sdmmc_card_t *g_card = nullptr;
State g_state = State::Off;

struct Lock {
    Lock() { xSemaphoreTake(g_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(g_lock); }
};

esp_vfs_fat_mount_config_t mount_config(bool format)
{
    esp_vfs_fat_mount_config_t config = VFS_FAT_MOUNT_DEFAULT_CONFIG();
    config.format_if_mount_failed = format;
    config.max_files = kMaxFiles;
    config.allocation_unit_size = kAllocationUnit;
    return config;
}

// ESP_FAIL from the mount is the FAT (none, or damaged); the other errors come from the card
esp_err_t mount_locked(bool format)
{
    if (g_card != nullptr) {
        return ESP_OK;
    }
    if (g_lock == nullptr || g_power == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = kMaxClockKhz;
    host.pwr_ctrl_handle = g_power;
    const esp_vfs_fat_mount_config_t config = mount_config(format);
    const int64_t started = esp_timer_get_time();
    const esp_err_t err = esp_vfs_fat_sdmmc_mount(kMountPoint, &host, &g_slot, &config, &g_card);
    if (err != ESP_OK) {
        g_card = nullptr;
        g_state = err == ESP_FAIL ? State::Unformatted : State::NoCard;
        if (g_state == State::Unformatted) {
            ESP_LOGW(kTag, "the card has no FAT that mounts: format it in the web UI");
        } else {
            ESP_LOGI(kTag, "no card (%s)", esp_err_to_name(err));
        }
        return g_state == State::Unformatted ? ESP_ERR_INVALID_STATE : ESP_ERR_NOT_FOUND;
    }
    g_state = State::Mounted;
    // the first count of the free space reads the whole FAT when the card has no valid FSInfo
    uint64_t total = 0;
    uint64_t free = 0;
    esp_vfs_fat_info(kMountPoint, &total, &free);
    ESP_LOGI(kTag, "%s at %s: %.1f of %.1f GB free, %d kHz, %lld ms", g_card->cid.name, kMountPoint,
             static_cast<double>(free) / 1e9, static_cast<double>(total) / 1e9, g_card->real_freq_khz,
             (esp_timer_get_time() - started) / 1000);
    return ESP_OK;
}

esp_err_t unmount_locked(State state)
{
    if (g_card == nullptr) {
        if (g_state == State::Mounted) {
            g_state = state;
        }
        return ESP_OK;
    }
    // removes slot 0 only: the controller stays while the C6's slot uses it
    const esp_err_t err = esp_vfs_fat_sdcard_unmount(kMountPoint, g_card);
    g_card = nullptr;
    g_state = state;
    return err;
}

} // namespace

esp_err_t start(const board::Info &board)
{
    if (g_lock != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (board.sd_clk_gpio < 0 || board.sd_cmd_gpio < 0 || board.sd_data_gpios[0] < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    g_lock = xSemaphoreCreateMutexStatic(&g_lock_buffer);
    if (board.sd_ldo_channel >= 0) {
        const sd_pwr_ctrl_ldo_config_t ldo{.ldo_chan_id = board.sd_ldo_channel};
        ESP_RETURN_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&ldo, &g_power), kTag, "LDO %d", board.sd_ldo_channel);
    }
    g_slot = SDMMC_SLOT_CONFIG_DEFAULT();
    g_slot.clk = static_cast<gpio_num_t>(board.sd_clk_gpio);
    g_slot.cmd = static_cast<gpio_num_t>(board.sd_cmd_gpio);
    g_slot.d0 = static_cast<gpio_num_t>(board.sd_data_gpios[0]);
    g_slot.d1 = static_cast<gpio_num_t>(board.sd_data_gpios[1]);
    g_slot.d2 = static_cast<gpio_num_t>(board.sd_data_gpios[2]);
    g_slot.d3 = static_cast<gpio_num_t>(board.sd_data_gpios[3]);
    g_slot.width = board.sd_data_gpios[3] >= 0 ? 4 : 1;
    // the board has its own pullups; the internal ones help a card that is slow to answer
    g_slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    g_state = State::NoCard;
    Lock lock;
    mount_locked(false);
    return ESP_OK;
}

Status status()
{
    Status out{};
    out.state = g_state;
    if (g_lock == nullptr) {
        return out;
    }
    // the screen and the web server ask: while a format or a mount runs they get the state alone
    if (xSemaphoreTake(g_lock, pdMS_TO_TICKS(kStatusWaitMs)) != pdTRUE) {
        return out;
    }
    out.state = g_state;
    if (g_card != nullptr) {
        esp_vfs_fat_info(kMountPoint, &out.total_bytes, &out.free_bytes);
        std::snprintf(out.name, sizeof(out.name), "%s", g_card->cid.name);
        out.clock_khz = static_cast<uint32_t>(g_card->real_freq_khz);
    }
    xSemaphoreGive(g_lock);
    return out;
}

const char *state_name(State state)
{
    switch (state) {
    case State::Off:
        return "off";
    case State::NoCard:
        return "no card";
    case State::Unformatted:
        return "not formatted";
    case State::Mounted:
        return "mounted";
    case State::Unmounted:
        return "unmounted";
    }
    return "?";
}

void describe(const Status &status, char *out, size_t size)
{
    if (status.state != State::Mounted) {
        std::snprintf(out, size, "%s", state_name(status.state));
        return;
    }
    const bool gb = status.total_bytes >= 1000000000ULL;
    const double unit = gb ? 1e9 : 1e6;
    std::snprintf(out, size, "%.1f of %.1f %s free", static_cast<double>(status.free_bytes) / unit,
                  static_cast<double>(status.total_bytes) / unit, gb ? "GB" : "MB");
}

esp_err_t mount()
{
    if (g_lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    Lock lock;
    return mount_locked(false);
}

esp_err_t unmount()
{
    if (g_lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    Lock lock;
    return unmount_locked(State::Unmounted);
}

esp_err_t format()
{
    if (g_lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    Lock lock;
    const int64_t started = esp_timer_get_time();
    esp_err_t err = ESP_OK;
    if (g_card != nullptr) {
        esp_vfs_fat_mount_config_t config = mount_config(false);
        err = esp_vfs_fat_sdcard_format_cfg(kMountPoint, g_card, &config);
    } else {
        // a card without a FAT: the mount formats it
        err = mount_locked(true);
    }
    if (err == ESP_OK) {
        ESP_LOGW(kTag, "card formatted on request (%lld ms)", (esp_timer_get_time() - started) / 1000);
    } else {
        ESP_LOGE(kTag, "format failed: %s", esp_err_to_name(err));
    }
    return err;
}

bool make_path(const char *relative, char *out, size_t size)
{
    while (*relative == '/') {
        relative++;
    }
    for (const char *p = relative; *p != 0; p++) {
        const auto c = static_cast<unsigned char>(*p);
        if (c < 0x20 || c == 0x7f || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|') {
            return false;
        }
        // ".." as a name (also "a/../b"), and an empty name ("a//b")
        if ((p == relative || p[-1] == '/') &&
            ((p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == 0)) || p[0] == '/')) {
            return false;
        }
    }
    const int n = *relative == 0 ? std::snprintf(out, size, "%s", kMountPoint)
                                 : std::snprintf(out, size, "%s/%s", kMountPoint, relative);
    if (n <= 0 || static_cast<size_t>(n) >= size || static_cast<size_t>(n) >= kMaxPath) {
        return false;
    }
    // a trailing '/' names the directory itself
    if (out[n - 1] == '/') {
        out[n - 1] = 0;
    }
    return true;
}

} // namespace hc::sdcard
