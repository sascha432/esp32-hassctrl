#include "hc/ota/ota.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hc/board/board.hpp"
#include "hc/platform/storage.hpp"

namespace hc::ota {

namespace {

constexpr const char *kTag = "ota";
// the project name of the main firmware (CMakeLists.txt); the recovery app is "hassctrl-recovery"
constexpr char kProject[] = "hassctrl";
// what has to arrive before a firmware image can be checked: image header, first segment header
// and the app description at the start of the first segment
// and the board tag right after it (hc::board::ImageTag)
constexpr size_t kFirmwareHead = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t) +
                                 sizeof(board::ImageTag);
// the LittleFS magic is in the first superblock, within the first 64 bytes
constexpr size_t kFsHead = 64;
constexpr size_t kSector = 4096;
constexpr size_t kBufferSize = 4096;
constexpr size_t kUrlSize = 256;
constexpr int kMaxRedirects = 5;

struct Job {
    Target target = Target::Firmware;
    const esp_partition_t *partition = nullptr;
    size_t size = 0;
    size_t written = 0; // bytes taken from the caller
    // the first bytes, kept until they are checked
    uint8_t head[kFirmwareHead] = {};
    size_t head_length = 0;
    bool head_checked = false;
    esp_ota_handle_t handle = 0;
    bool ota_begun = false;
    size_t erased = 0; // file system: bytes of the partition erased so far
    size_t flushed = 0; // file system: bytes written to the partition
};

Job g_job;
// whether ota_0 holds a complete, valid image: -1 not checked yet. Checking reads the whole image,
// so it is done once and again after an update.
int g_firmware_valid = -1;
void (*g_on_start)(Target) = nullptr;
Status g_status{State::Idle, Target::Firmware, 0, 0, {}};
portMUX_TYPE g_status_lock = portMUX_INITIALIZER_UNLOCKED;

bool busy()
{
    taskENTER_CRITICAL(&g_status_lock);
    const bool result = g_status.state == State::Receiving || g_status.state == State::Downloading;
    taskEXIT_CRITICAL(&g_status_lock);
    return result;
}

void set_progress(size_t done)
{
    taskENTER_CRITICAL(&g_status_lock);
    g_status.done = done;
    taskEXIT_CRITICAL(&g_status_lock);
}

void set_state(State state)
{
    taskENTER_CRITICAL(&g_status_lock);
    g_status.state = state;
    taskEXIT_CRITICAL(&g_status_lock);
}

// Marks the update as failed with a reason in words and returns `err`.
esp_err_t fail(esp_err_t err, const char *format, ...)
{
    char text[sizeof(g_status.error)];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    ESP_LOGE(kTag, "%s update failed: %s", target_name(g_job.target), text);
    taskENTER_CRITICAL(&g_status_lock);
    g_status.state = State::Failed;
    std::memcpy(g_status.error, text, sizeof(text));
    taskEXIT_CRITICAL(&g_status_lock);
    return err;
}

const esp_partition_t *firmware_partition()
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
}

const esp_partition_t *fs_partition()
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, platform::kFsPartition);
}

bool firmware_valid()
{
    if (g_firmware_valid < 0) {
        const esp_partition_t *firmware = firmware_partition();
        const esp_partition_pos_t position{firmware != nullptr ? firmware->address : 0,
                                           firmware != nullptr ? firmware->size : 0};
        esp_image_metadata_t metadata{};
        g_firmware_valid =
            firmware != nullptr && esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &position, &metadata) == ESP_OK ? 1 : 0;
    }
    return g_firmware_valid == 1;
}

// the partition a firmware update goes to, nullptr when this app runs from it
const esp_partition_t *firmware_target()
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
    return next != esp_ota_get_running_partition() ? next : nullptr;
}

// `reserved`: start_download() has marked the update as running already
esp_err_t start_job(Target target, size_t size, State state, bool reserved = false)
{
    if (!reserved && busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    g_job = {};
    g_job.target = target;
    g_job.size = size;
    taskENTER_CRITICAL(&g_status_lock);
    g_status = {state, target, 0, size, {}};
    taskEXIT_CRITICAL(&g_status_lock);

    if (target == Target::Firmware) {
        g_firmware_valid = -1;
        g_job.partition = firmware_target();
        if (g_job.partition == nullptr) {
            return fail(ESP_ERR_NOT_SUPPORTED, "this firmware cannot replace itself, update from the recovery app");
        }
    } else {
        g_job.partition = fs_partition();
        if (g_job.partition == nullptr) {
            return fail(ESP_ERR_NOT_FOUND, "no file system partition");
        }
    }
    if (size > g_job.partition->size) {
        return fail(ESP_ERR_INVALID_SIZE, "the image (%u KB) is larger than the partition (%u KB)",
                    static_cast<unsigned>(size / 1024), static_cast<unsigned>(g_job.partition->size / 1024));
    }
    ESP_LOGI(kTag, "%s update to '%s', %u bytes", target_name(target), g_job.partition->label, static_cast<unsigned>(size));
    if (g_on_start != nullptr) {
        g_on_start(target);
    }
    return ESP_OK;
}

esp_err_t check_firmware_head()
{
    const auto *image = reinterpret_cast<const esp_image_header_t *>(g_job.head);
    if (image->magic != ESP_IMAGE_HEADER_MAGIC) {
        return fail(ESP_ERR_INVALID_ARG, "not a firmware image");
    }
    if (image->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        return fail(ESP_ERR_INVALID_ARG, "the firmware is for another chip");
    }
    const auto *app = reinterpret_cast<const esp_app_desc_t *>(g_job.head + sizeof(esp_image_header_t) +
                                                               sizeof(esp_image_segment_header_t));
    if (app->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return fail(ESP_ERR_INVALID_ARG, "the firmware has no app description");
    }
    if (strncmp(app->project_name, kProject, sizeof(app->project_name)) != 0) {
        return fail(ESP_ERR_INVALID_ARG, "'%.32s' is not the panel firmware", app->project_name);
    }
    board::ImageTag tag{};
    std::memcpy(&tag, reinterpret_cast<const uint8_t *>(app) + sizeof(esp_app_desc_t), sizeof(tag));
    if (tag.magic != board::kImageTagMagic) {
        return fail(ESP_ERR_INVALID_ARG, "the firmware does not say which board it is for");
    }
    if (strncmp(tag.board, board::info().name, sizeof(tag.board)) != 0) {
        return fail(ESP_ERR_INVALID_ARG, "the firmware is for the %.32s, this is the %s", tag.board, board::info().name);
    }
    ESP_LOGI(kTag, "firmware %.32s %.32s, built %.16s %.16s", app->project_name, app->version, app->date, app->time);
    const esp_err_t err = esp_ota_begin(g_job.partition, OTA_WITH_SEQUENTIAL_WRITES, &g_job.handle);
    if (err != ESP_OK) {
        return fail(err, "cannot start writing (%s)", esp_err_to_name(err));
    }
    g_job.ota_begun = true;
    return ESP_OK;
}

esp_err_t check_fs_head()
{
    constexpr char kMagic[] = "littlefs";
    for (size_t i = 0; i + sizeof(kMagic) - 1 <= kFsHead; i++) {
        if (std::memcmp(g_job.head + i, kMagic, sizeof(kMagic) - 1) == 0) {
            const esp_err_t err = platform::unmount_fs();
            if (err != ESP_OK) {
                return fail(err, "the file system cannot be unmounted");
            }
            return ESP_OK;
        }
    }
    return fail(ESP_ERR_INVALID_ARG, "not a LittleFS image");
}

esp_err_t write_out(const uint8_t *data, size_t size)
{
    if (g_job.target == Target::Firmware) {
        const esp_err_t err = esp_ota_write(g_job.handle, data, size);
        return err == ESP_OK ? ESP_OK : fail(err, "writing failed (%s)", esp_err_to_name(err));
    }
    if (g_job.flushed + size > g_job.partition->size) {
        return fail(ESP_ERR_INVALID_SIZE, "the image is larger than the partition");
    }
    // erases ahead of the writes, one sector at a time, so the first bytes go out at once
    while (g_job.erased < g_job.flushed + size) {
        const esp_err_t err = esp_partition_erase_range(g_job.partition, g_job.erased, kSector);
        if (err != ESP_OK) {
            return fail(err, "erasing failed (%s)", esp_err_to_name(err));
        }
        g_job.erased += kSector;
    }
    const esp_err_t err = esp_partition_write(g_job.partition, g_job.flushed, data, size);
    if (err != ESP_OK) {
        return fail(err, "writing failed (%s)", esp_err_to_name(err));
    }
    g_job.flushed += size;
    return ESP_OK;
}

esp_err_t restart_into(const esp_partition_t *partition, uint32_t delay_ms)
{
    if (partition == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "cannot boot '%s': %s", partition->label, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(kTag, "restarting into '%s'", partition->label);
    restart_later(delay_ms);
    return ESP_OK;
}

// ---------------------------------------------------------------------------------- download

struct Download {
    Target target;
    char url[kUrlSize];
};

esp_err_t download(const Download &job)
{
    esp_http_client_config_t config{};
    config.url = job.url;
    config.timeout_ms = 15000;
    config.buffer_size = 2048;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return fail(ESP_ERR_NO_MEM, "no memory for the download");
    }

    esp_err_t err = ESP_OK;
    int64_t length = 0;
    // redirects are followed by hand: open() and read() do not follow them
    for (int redirects = 0;; redirects++) {
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            esp_http_client_cleanup(client);
            return fail(err, "cannot connect (%s)", esp_err_to_name(err));
        }
        length = esp_http_client_fetch_headers(client);
        const int code = esp_http_client_get_status_code(client);
        if (code >= 300 && code < 400 && redirects < kMaxRedirects) {
            esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            continue;
        }
        if (code != 200) {
            esp_http_client_cleanup(client);
            return fail(ESP_FAIL, "the server answered %d", code);
        }
        break;
    }

    err = start_job(job.target, length > 0 ? static_cast<size_t>(length) : 0, State::Downloading, true);
    auto *buffer = static_cast<uint8_t *>(std::malloc(kBufferSize));
    if (err == ESP_OK && buffer == nullptr) {
        err = fail(ESP_ERR_NO_MEM, "no memory for the download");
    }
    while (err == ESP_OK) {
        const int n = esp_http_client_read(client, reinterpret_cast<char *>(buffer), kBufferSize);
        if (n < 0) {
            err = fail(ESP_FAIL, "the download broke off");
        } else if (n == 0) {
            if (!esp_http_client_is_complete_data_received(client)) {
                err = fail(ESP_FAIL, "the download broke off");
            }
            break;
        } else {
            err = write(buffer, static_cast<size_t>(n));
        }
    }
    std::free(buffer);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        abort();
        return err;
    }
    return finish();
}

void download_task(void *arg)
{
    auto *job = static_cast<Download *>(arg);
    const bool firmware = job->target == Target::Firmware;
    if (download(*job) == ESP_OK && firmware) {
        restart_later(1000);
    }
    delete job;
    vTaskDelete(nullptr);
}

} // namespace

void set_on_start(void (*on_start)(Target target))
{
    g_on_start = on_start;
}

Info info()
{
    Info result{};
    const esp_partition_t *running = esp_ota_get_running_partition();
    result.recovery_app = running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY;
    esp_ota_img_states_t running_state = ESP_OTA_IMG_UNDEFINED;
    result.confirmed = result.recovery_app || esp_ota_get_state_partition(running, &running_state) != ESP_OK ||
                       running_state != ESP_OTA_IMG_PENDING_VERIFY;
    const esp_partition_t *target = firmware_target();
    result.firmware_direct = target != nullptr;
    const esp_partition_t *firmware = firmware_partition();
    esp_app_desc_t app{};
    if (firmware != nullptr && esp_ota_get_partition_description(firmware, &app) == ESP_OK && firmware_valid()) {
        result.firmware_present = true;
        esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
        result.firmware_failed = esp_ota_get_state_partition(firmware, &state) == ESP_OK &&
                                 (state == ESP_OTA_IMG_ABORTED || state == ESP_OTA_IMG_INVALID);
        std::snprintf(result.firmware_version, sizeof(result.firmware_version), "%.31s", app.version);
    }
    result.firmware_max = target != nullptr ? target->size : (firmware != nullptr ? firmware->size : 0);
    const esp_partition_t *fs = fs_partition();
    result.filesystem_max = fs != nullptr ? fs->size : 0;
    return result;
}

Status status()
{
    taskENTER_CRITICAL(&g_status_lock);
    const Status result = g_status;
    taskEXIT_CRITICAL(&g_status_lock);
    return result;
}

esp_err_t begin(Target target, size_t size)
{
    return start_job(target, size, State::Receiving);
}

esp_err_t write(const void *data, size_t size)
{
    const auto *bytes = static_cast<const uint8_t *>(data);
    if (status().state == State::Failed) {
        return ESP_FAIL;
    }
    if (g_job.size != 0 && g_job.written + size > g_job.size) {
        return fail(ESP_ERR_INVALID_SIZE, "more data than announced");
    }
    g_job.written += size;
    set_progress(g_job.written);

    if (!g_job.head_checked) {
        const size_t wanted = g_job.target == Target::Firmware ? kFirmwareHead : kFsHead;
        const size_t take = std::min(size, wanted - g_job.head_length);
        std::memcpy(g_job.head + g_job.head_length, bytes, take);
        g_job.head_length += take;
        bytes += take;
        size -= take;
        if (g_job.head_length < wanted) {
            return ESP_OK;
        }
        g_job.head_checked = true;
        esp_err_t err = g_job.target == Target::Firmware ? check_firmware_head() : check_fs_head();
        if (err == ESP_OK) {
            err = write_out(g_job.head, g_job.head_length);
        }
        if (err != ESP_OK) {
            return err;
        }
    }
    return size > 0 ? write_out(bytes, size) : ESP_OK;
}

esp_err_t finish()
{
    if (status().state == State::Failed) {
        return ESP_FAIL;
    }
    if (!g_job.head_checked) {
        abort();
        return fail(ESP_ERR_INVALID_SIZE, "the image is too short");
    }
    if (g_job.size != 0 && g_job.written != g_job.size) {
        abort();
        return fail(ESP_ERR_INVALID_SIZE, "the image is incomplete");
    }

    if (g_job.target == Target::Firmware) {
        esp_err_t err = esp_ota_end(g_job.handle);
        g_job.ota_begun = false;
        if (err != ESP_OK) {
            return fail(err, err == ESP_ERR_OTA_VALIDATE_FAILED ? "the image is damaged" : "checking the image failed (%s)",
                        esp_err_to_name(err));
        }
        err = esp_ota_set_boot_partition(g_job.partition);
        if (err != ESP_OK) {
            return fail(err, "cannot select the new firmware (%s)", esp_err_to_name(err));
        }
    } else {
        // the rest of the partition must not hold old blocks
        if (g_job.erased < g_job.partition->size) {
            const esp_err_t err =
                esp_partition_erase_range(g_job.partition, g_job.erased, g_job.partition->size - g_job.erased);
            if (err != ESP_OK) {
                return fail(err, "erasing failed (%s)", esp_err_to_name(err));
            }
        }
        if (platform::mount_fs_again() != platform::FsState::Mounted) {
            return fail(ESP_FAIL, "written, but the new file system does not mount");
        }
    }
    ESP_LOGI(kTag, "%s update done, %u bytes", target_name(g_job.target), static_cast<unsigned>(g_job.written));
    set_state(State::Done);
    return ESP_OK;
}

void abort()
{
    if (g_job.ota_begun) {
        esp_ota_abort(g_job.handle);
        g_job.ota_begun = false;
    }
    if (g_job.target == Target::FileSystem && g_job.head_checked) {
        // the old file system is partly overwritten; mounting shows what is left (recovery mode)
        platform::mount_fs_again();
    }
    if (status().state != State::Failed) {
        fail(ESP_FAIL, "aborted");
    }
}

esp_err_t start_download(Target target, const char *url)
{
    if (busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (std::strncmp(url, "http://", 7) != 0 && std::strncmp(url, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    auto *job = new Download{target, {}};
    std::snprintf(job->url, sizeof(job->url), "%s", url);
    taskENTER_CRITICAL(&g_status_lock);
    g_status = {State::Downloading, target, 0, 0, {}};
    taskEXIT_CRITICAL(&g_status_lock);
    // TLS needs the larger stack
    if (xTaskCreate(download_task, "ota_download", 8192, job, 5, nullptr) != pdPASS) {
        delete job;
        set_state(State::Idle);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t restart_into_recovery(uint32_t delay_ms)
{
    return restart_into(esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr),
                        delay_ms);
}

esp_err_t restart_into_firmware(uint32_t delay_ms)
{
    if (!info().firmware_present) {
        return ESP_ERR_NOT_FOUND;
    }
    return restart_into(firmware_partition(), delay_ms);
}

void restart_later(uint32_t delay_ms)
{
    esp_timer_create_args_t args{};
    args.callback = [](void *) { esp_restart(); };
    args.name = "restart";
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&args, &timer) != ESP_OK || esp_timer_start_once(timer, delay_ms * 1000ull) != ESP_OK) {
        esp_restart();
    }
}

const char *target_name(Target target)
{
    return target == Target::Firmware ? "firmware" : "filesystem";
}

const char *state_name(State state)
{
    switch (state) {
    case State::Idle:
        return "idle";
    case State::Receiving:
        return "receiving";
    case State::Downloading:
        return "downloading";
    case State::Done:
        return "done";
    case State::Failed:
        return "failed";
    }
    return "?";
}

} // namespace hc::ota
