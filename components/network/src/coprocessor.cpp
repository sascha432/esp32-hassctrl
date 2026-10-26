#include "hc/network/coprocessor.hpp"

#include <algorithm>
#include <cstdio>

#include "sdkconfig.h"

#if CONFIG_ESP_HOSTED
#include "eh_common_fw_version.h"
#include "eh_host_cp_ota.h"
#include "eh_host_sys.h"
#include "esp_app_format.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_wifi.h"
#endif

namespace hc::network {

#if CONFIG_ESP_HOSTED

namespace {

constexpr const char *kTag = "coprocessor";

// the largest piece one OTA write RPC takes (EH_RPC_OTA_CHUNK_MAX)
constexpr int kChunk = 1536;

#define HC_STR2(x) #x
#define HC_STR(x) HC_STR2(x)
constexpr const char *kHostVersion =
    HC_STR(PROJECT_VERSION_MAJOR_1) "." HC_STR(PROJECT_VERSION_MINOR_1) "." HC_STR(PROJECT_VERSION_PATCH_1);

// Coprocessor firmware before 2.6.0 marks the new image for boot in ota_end; later ones need the
// separate activate call (esp_hosted examples/ota/coprocessor_ota)
bool needs_activate(const eh_host_coprocessor_fwver_t &v)
{
    return v.major1 > 2 || (v.major1 == 2 && v.minor1 >= 6);
}

esp_err_t download(esp_http_client_handle_t client, CoprocessorProgress progress)
{
    ESP_RETURN_ON_ERROR(esp_http_client_open(client, 0), kTag, "connect");
    const int64_t length = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(kTag, "HTTP status %d", status);
        return ESP_ERR_NOT_FOUND;
    }
    const size_t total = length > 0 ? static_cast<size_t>(length) : 0;

    ESP_RETURN_ON_ERROR(eh_host_cp_ota_begin(), kTag, "OTA begin");
    static uint8_t buffer[kChunk];
    size_t done = 0;
    size_t reported = 0;
    for (;;) {
        const int n = esp_http_client_read(client, reinterpret_cast<char *>(buffer), kChunk);
        if (n < 0) {
            ESP_LOGE(kTag, "download failed after %u bytes", static_cast<unsigned>(done));
            eh_host_cp_ota_end();
            return ESP_FAIL;
        }
        if (n == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            continue;
        }
        ESP_RETURN_ON_ERROR(eh_host_cp_ota_write(buffer, static_cast<uint32_t>(n)), kTag, "OTA write at %u",
                            static_cast<unsigned>(done));
        done += static_cast<size_t>(n);
        // every 64 KB
        if (progress != nullptr && done - reported >= 64 * 1024) {
            reported = done;
            progress(done, total);
        }
    }
    if (progress != nullptr) {
        progress(done, total);
    }
    if (total != 0 && done != total) {
        ESP_LOGE(kTag, "image cut short: %u of %u bytes", static_cast<unsigned>(done), static_cast<unsigned>(total));
        eh_host_cp_ota_end();
        return ESP_ERR_INVALID_SIZE;
    }
    // the coprocessor checks the image here and refuses a broken one
    return eh_host_cp_ota_end();
}

// The length of the ESP app image at the start of a partition: header, segments, the checksum
// padded to 16 bytes and the SHA-256 when appended (the layout of esp_image_format.c). 0: none.
size_t image_length(const esp_partition_t *partition)
{
    esp_image_header_t header{};
    if (esp_partition_read(partition, 0, &header, sizeof(header)) != ESP_OK || header.magic != ESP_IMAGE_HEADER_MAGIC ||
        header.segment_count == 0 || header.segment_count > ESP_IMAGE_MAX_SEGMENTS) {
        return 0;
    }
    size_t offset = sizeof(header);
    for (uint8_t i = 0; i < header.segment_count; ++i) {
        esp_image_segment_header_t segment{};
        if (esp_partition_read(partition, offset, &segment, sizeof(segment)) != ESP_OK) {
            return 0;
        }
        offset += sizeof(segment) + segment.data_len;
        if (offset > partition->size) {
            return 0;
        }
    }
    offset = (offset + 16) & ~static_cast<size_t>(15);
    if (header.hash_appended) {
        offset += 32;
    }
    return offset <= partition->size ? offset : 0;
}

esp_err_t write_from_partition(const esp_partition_t *partition, size_t total, CoprocessorProgress progress)
{
    ESP_RETURN_ON_ERROR(eh_host_cp_ota_begin(), kTag, "OTA begin");
    static uint8_t buffer[kChunk];
    size_t reported = 0;
    for (size_t done = 0; done < total;) {
        const size_t n = std::min(total - done, sizeof(buffer));
        if (esp_partition_read(partition, done, buffer, n) != ESP_OK) {
            eh_host_cp_ota_end();
            return ESP_FAIL;
        }
        ESP_RETURN_ON_ERROR(eh_host_cp_ota_write(buffer, static_cast<uint32_t>(n)), kTag, "OTA write at %u",
                            static_cast<unsigned>(done));
        done += n;
        if (progress != nullptr && (done - reported >= 64 * 1024 || done == total)) {
            reported = done;
            progress(done, total);
        }
    }
    return eh_host_cp_ota_end();
}

esp_err_t finish(const eh_host_coprocessor_fwver_t &version)
{
    if (needs_activate(version)) {
        ESP_RETURN_ON_ERROR(eh_host_cp_ota_activate(), kTag, "activate");
    }
    ESP_LOGI(kTag, "new firmware written, it starts with the next reset of the coprocessor");
    return ESP_OK;
}

} // namespace

bool has_coprocessor()
{
    return true;
}

esp_err_t coprocessor_version(char *text, size_t size)
{
    eh_host_coprocessor_fwver_t v{};
    ESP_RETURN_ON_ERROR(eh_host_sys_get_cp_fw_version(&v), kTag, "version");
    std::snprintf(text, size, "%u.%u.%u", static_cast<unsigned>(v.major1), static_cast<unsigned>(v.minor1),
                  static_cast<unsigned>(v.patch1));
    return ESP_OK;
}

const char *host_hosted_version()
{
    return kHostVersion;
}

esp_err_t update_coprocessor(const char *url, CoprocessorProgress progress)
{
    eh_host_coprocessor_fwver_t version{};
    ESP_RETURN_ON_ERROR(eh_host_sys_get_cp_fw_version(&version), kTag, "version");

    esp_http_client_config_t config{};
    config.url = url;
    config.timeout_ms = 10000;
    config.buffer_size = kChunk;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = download(client, progress);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_RETURN_ON_ERROR(err, kTag, "update");
    return finish(version);
}

const char *coprocessor_slot()
{
    const esp_partition_t *slot = esp_ota_get_next_update_partition(nullptr);
    return slot != nullptr ? slot->label : "";
}

esp_err_t update_coprocessor_from_slot(CoprocessorProgress progress)
{
    eh_host_coprocessor_fwver_t version{};
    ESP_RETURN_ON_ERROR(eh_host_sys_get_cp_fw_version(&version), kTag, "version");
    const esp_partition_t *slot = esp_ota_get_next_update_partition(nullptr);
    const size_t total = slot != nullptr ? image_length(slot) : 0;
    if (total == 0) {
        ESP_LOGE(kTag, "no app image in %s", slot != nullptr ? slot->label : "the free app slot");
        return ESP_ERR_NOT_FOUND;
    }
    // no station traffic over SDIO while the image goes across; the caller restarts afterwards
    esp_wifi_stop();
    ESP_RETURN_ON_ERROR(write_from_partition(slot, total, progress), kTag, "update");
    return finish(version);
}

#else

bool has_coprocessor()
{
    return false;
}

esp_err_t coprocessor_version(char *, size_t)
{
    return ESP_ERR_NOT_SUPPORTED;
}

const char *host_hosted_version()
{
    return "";
}

esp_err_t update_coprocessor(const char *, CoprocessorProgress)
{
    return ESP_ERR_NOT_SUPPORTED;
}

const char *coprocessor_slot()
{
    return "";
}

esp_err_t update_coprocessor_from_slot(CoprocessorProgress)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif

} // namespace hc::network
