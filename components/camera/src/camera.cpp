// The MIPI-CSI camera: capture and settings (hc/camera/camera.hpp).
//
// Capture: V4L2 on esp_video's CSI device, three MMAP buffers in PSRAM. The task keeps the newest
// frame dequeued and gives the previous one back, so readers use the frame where the ISP wrote it
// (no copy) while the CSI fills the other two. Readers hold g_mutex while they use the frame.
//
// Picture settings: mirror and flip are sensor registers (V4L2 HFLIP / VFLIP); the exposure
// control (esp_ipa's AGC in the ISP pipeline) runs or is stopped and the exposure and gain are set
// by hand (V4L2 EXPOSURE in lines, GAIN as an index of the driver's gain table in 1/16); the
// brightness and the mains frequency change a copy of the AGC's tuning (the OV02C10's JSON).

#include "hc/camera/camera.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "camera_private.hpp"
#include "esp_cam_sensor_types.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_video_ioctl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "esp_video_isp_pipeline.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hc/platform/settings.hpp"
#include "linux/videodev2.h"

namespace hc::camera {
namespace {

constexpr const char *kTag = "camera";
constexpr int kBufferCount = 3;
constexpr uint32_t kSccbHz = 100000; // the bus of the touch controller and the codec
constexpr const char *kSensorName = "OV02C10"; // the name of its IPA tuning

constexpr const char *kNamespace = "camera";
constexpr const char *kPictureKey = "picture";
constexpr const char *kStreamKey = "stream";
constexpr const char *kUserKey = "user";
constexpr const char *kPasswordKey = "password";

int g_fd = -1;
uint16_t g_width = 0;
uint16_t g_height = 0;
uint32_t g_tline_ns = 0;      // one line of exposure
uint32_t g_max_exposure = 0;  // in lines
TaskHandle_t g_task = nullptr;
std::atomic<int> g_users{0};
// Mirror and flip change the sensor's readout window: written while it streams, the CSI got a
// frame of another shape and never delivered one again. They are written in standby only; a
// change while the capture runs restarts it (g_restart).
std::atomic<bool> g_capturing{false};
std::atomic<bool> g_restart{false};
constexpr int kFrameTimeoutMs = 1000; // no frame for this long: the capture is started again

std::mutex g_mutex;
std::condition_variable g_new_frame;
uint8_t *g_buffers[kBufferCount] = {};
int g_current = -1;        // the dequeued buffer with the newest frame, -1: none
uint32_t g_generation = 0; // of the newest frame

std::mutex g_settings_mutex;
Picture g_picture;
Stream g_stream;
// the AGC's tuning as built from the JSON, and the copy with the settings applied
const esp_ipa_config_t *g_ipa_base = nullptr;
esp_ipa_config_t g_ipa;
esp_ipa_agc_config_t g_agc;

bool queue(int index)
{
    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    return ioctl(g_fd, VIDIOC_QBUF, &buf) == 0;
}

bool set_control(uint32_t id, int32_t value);
Picture picture_now();

bool stream_on()
{
    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = g_width;
    format.fmt.pix.height = g_height;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    if (ioctl(g_fd, VIDIOC_S_FMT, &format) != 0) {
        ESP_LOGE(kTag, "RGB565 %ux%u not taken", g_width, g_height);
        return false;
    }
    v4l2_requestbuffers request{};
    request.count = kBufferCount;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (ioctl(g_fd, VIDIOC_REQBUFS, &request) != 0) {
        ESP_LOGE(kTag, "no buffers");
        return false;
    }
    for (int i = 0; i < kBufferCount; i++) {
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(g_fd, VIDIOC_QUERYBUF, &buf) != 0) {
            return false;
        }
        g_buffers[i] = static_cast<uint8_t *>(mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, buf.m.offset));
        if (g_buffers[i] == nullptr || !queue(i)) {
            return false;
        }
    }
    // the sensor is in standby until STREAMON: the readout's turn now
    const Picture picture = picture_now();
    set_control(V4L2_CID_HFLIP, picture.mirror ? 1 : 0);
    set_control(V4L2_CID_VFLIP, picture.flip ? 1 : 0);
    const timeval timeout{.tv_sec = 0, .tv_usec = kFrameTimeoutMs * 1000};
    ioctl(g_fd, VIDIOC_S_DQBUF_TIMEOUT, &timeout);
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    return ioctl(g_fd, VIDIOC_STREAMON, &type) == 0;
}

void stream_off()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_current = -1;
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(g_fd, VIDIOC_STREAMOFF, &type);
}

void capture_task(void *)
{
    for (;;) {
        if (g_users.load() <= 0) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        g_restart = false;
        g_capturing = true;
        if (!stream_on()) {
            ESP_LOGE(kTag, "capture did not start");
            stream_off();
            g_capturing = false;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        ESP_LOGI(kTag, "capture on");
        uint32_t frames = 0;
        int64_t since = esp_timer_get_time();
        while (g_users.load() > 0 && !g_restart) {
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            if (ioctl(g_fd, VIDIOC_DQBUF, &buf) != 0) {
                ESP_LOGW(kTag, "no frame for %d ms: the capture starts again", kFrameTimeoutMs);
                break;
            }
            if ((buf.flags & V4L2_BUF_FLAG_DONE) == 0) {
                queue(buf.index);
                continue;
            }
            int previous;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                previous = g_current;
                g_current = static_cast<int>(buf.index);
                g_generation++;
            }
            g_new_frame.notify_all();
            if (previous >= 0) {
                queue(previous);
            }
            frames++;
            const int64_t now = esp_timer_get_time();
            if (now - since >= 60 * 1000 * 1000) {
                ESP_LOGI(kTag, "%.1f fps", frames * 1e6 / (now - since));
                frames = 0;
                since = now;
            }
        }
        stream_off();
        g_capturing = false;
        ESP_LOGI(kTag, "capture off");
    }
}

bool set_control(uint32_t id, int32_t value)
{
    v4l2_ext_control control{};
    control.id = id;
    control.value = value;
    v4l2_ext_controls controls{};
    controls.ctrl_class = V4L2_CID_CAMERA_CLASS;
    controls.count = 1;
    controls.controls = &control;
    if (ioctl(g_fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
        ESP_LOGW(kTag, "control 0x%" PRIx32 " = %" PRIi32 " not taken", id, value);
        return false;
    }
    return true;
}

// the AGC's tuning with the brightness and the mains frequency of `picture`
void apply_tuning(const Picture &picture)
{
    if (g_ipa_base == nullptr || g_ipa_base->agc == nullptr) {
        return;
    }
    g_agc = *g_ipa_base->agc;
    auto shift = [&](uint8_t luma) {
        return static_cast<uint8_t>(std::clamp(int{luma} + picture.brightness, 3, 252));
    };
    g_agc.luma_low = shift(g_agc.luma_low);
    g_agc.luma_high = shift(g_agc.luma_high);
    g_agc.luma_target = shift(g_agc.luma_target);
    g_agc.ac_freq = picture.mains_hz;
    g_ipa = *g_ipa_base;
    g_ipa.agc = &g_agc;
    if (esp_video_isp_pipeline_set_ipa_config(&g_ipa) != ESP_OK) {
        ESP_LOGW(kTag, "exposure tuning not taken");
    }
}

void apply(const Picture &picture, const Picture *before)
{
    if (before == nullptr || picture.mirror != before->mirror || picture.flip != before->flip) {
        if (g_capturing) {
            g_restart = true; // stream_on() writes them
        } else {
            set_control(V4L2_CID_HFLIP, picture.mirror ? 1 : 0);
            set_control(V4L2_CID_VFLIP, picture.flip ? 1 : 0);
        }
    }
    // the tuning only when it differs from the built one (at the start) or changed
    const bool tuned = picture.brightness != 0 ||
                       (g_ipa_base != nullptr && g_ipa_base->agc != nullptr && picture.mains_hz != g_ipa_base->agc->ac_freq);
    if (before == nullptr ? tuned : (picture.brightness != before->brightness || picture.mains_hz != before->mains_hz)) {
        apply_tuning(picture);
    }
    if (before == nullptr || picture.auto_exposure != before->auto_exposure) {
        esp_video_isp_pipeline_set_agc_status(picture.auto_exposure ? ESP_VIDEO_ISP_PIPELINE_AGC_ENABLE
                                                                    : ESP_VIDEO_ISP_PIPELINE_AGC_DISABLE);
    }
    if (picture.auto_exposure) {
        return;
    }
    if (before == nullptr || before->auto_exposure || picture.exposure_us != before->exposure_us) {
        const uint32_t lines = static_cast<uint32_t>(uint64_t{picture.exposure_us} * 1000 / std::max<uint32_t>(g_tline_ns, 1));
        set_control(V4L2_CID_EXPOSURE, static_cast<int32_t>(std::clamp<uint32_t>(lines, 1, g_max_exposure)));
    }
    if (before == nullptr || before->auto_exposure || picture.gain != before->gain) {
        set_control(V4L2_CID_GAIN, picture.gain - kMinGain);
    }
}

Picture picture_now()
{
    std::lock_guard<std::mutex> lock(g_settings_mutex);
    return g_picture;
}

Picture valid(Picture p)
{
    p.brightness = std::clamp<int8_t>(p.brightness, -kMaxBrightness, kMaxBrightness);
    p.exposure_us = std::clamp<uint16_t>(p.exposure_us, kMinExposureUs, std::max(max_exposure_us(), kMinExposureUs));
    p.gain = std::clamp(p.gain, kMinGain, kMaxGain);
    p.mains_hz = p.mains_hz == 50 ? 50 : 60;
    return p;
}

// the stream's settings without the login, as stored
struct StoredStream {
    bool enabled;
    bool half;
    uint8_t max_fps;
    uint8_t quality;
};

void load()
{
    platform::Settings settings(kNamespace, platform::Settings::Mode::Read);
    if (!settings.ok()) {
        return;
    }
    Picture picture{};
    size_t size = sizeof(picture);
    if (settings.get_blob(kPictureKey, &picture, size) == ESP_OK && size == sizeof(picture)) {
        g_picture = picture;
    }
    StoredStream stored{};
    size = sizeof(stored);
    if (settings.get_blob(kStreamKey, &stored, size) == ESP_OK && size == sizeof(stored)) {
        g_stream.enabled = stored.enabled;
        g_stream.half = stored.half;
        g_stream.max_fps = stored.max_fps;
        g_stream.quality = stored.quality;
    }
    settings.get_string(kUserKey, g_stream.username, sizeof(g_stream.username));
    settings.get_string(kPasswordKey, g_stream.password, sizeof(g_stream.password));
}

} // namespace

esp_err_t start(i2c_master_bus_handle_t bus)
{
    esp_video_init_csi_config_t csi{};
    csi.sccb_config.init_sccb = false;
    csi.sccb_config.i2c_handle = bus;
    csi.sccb_config.freq = kSccbHz;
    csi.reset_pin = GPIO_NUM_NC;
    csi.pwdn_pin = GPIO_NUM_NC;
    // the board holds the MIPI PHY supply (LDO channel 3, 2.5 V) for the display
    csi.dont_init_ldo = true;
    esp_video_init_config_t config{};
    config.csi = &csi;
    ESP_RETURN_ON_ERROR(esp_video_init(&config), kTag, "no sensor");

    g_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (g_fd < 0) {
        ESP_LOGW(kTag, "no sensor");
        return ESP_ERR_NOT_FOUND;
    }
    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    esp_cam_sensor_format_t sensor_format{};
    if (ioctl(g_fd, VIDIOC_G_FMT, &format) != 0 || ioctl(g_fd, VIDIOC_G_SENSOR_FMT, &sensor_format) != 0) {
        close(g_fd);
        g_fd = -1;
        return ESP_FAIL;
    }
    g_width = static_cast<uint16_t>(format.fmt.pix.width);
    g_height = static_cast<uint16_t>(format.fmt.pix.height);
    g_tline_ns = sensor_format.isp_info != nullptr ? sensor_format.isp_info->isp_v1_info.tline_ns : 0;
    v4l2_query_ext_ctrl query{};
    query.id = V4L2_CID_EXPOSURE;
    g_max_exposure = ioctl(g_fd, VIDIOC_QUERY_EXT_CTRL, &query) == 0 ? static_cast<uint32_t>(query.maximum) : 1;
    g_ipa_base = esp_video_isp_pipeline_enum_ipa_configs(kSensorName, 0);
    ESP_LOGI(kTag, "sensor %s, %s, %ux%u as RGB565, exposure up to %u us", kSensorName, sensor_format.name, g_width,
             g_height, max_exposure_us());

    load();
    detail::login_changed(g_stream);
    g_picture = valid(g_picture);
    apply(g_picture, nullptr);

    // core 0: the CSI's DMA interrupt shares the DW-GDMA interrupt group with the display's DPI
    // channel, which app_main created on core 0; from core 1 the allocation fails
    if (xTaskCreatePinnedToCore(capture_task, "camera", 4096, nullptr, 3, &g_task, 0) != pdPASS) {
        close(g_fd);
        g_fd = -1;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool available()
{
    return g_task != nullptr;
}

uint16_t width()
{
    return g_width;
}

uint16_t height()
{
    return g_height;
}

uint16_t max_exposure_us()
{
    return static_cast<uint16_t>(std::min<uint64_t>(uint64_t{g_max_exposure} * g_tline_ns / 1000, UINT16_MAX));
}

Picture picture()
{
    std::lock_guard<std::mutex> lock(g_settings_mutex);
    return g_picture;
}

void set_picture(const Picture &picture)
{
    std::lock_guard<std::mutex> lock(g_settings_mutex);
    const Picture before = g_picture;
    g_picture = valid(picture);
    if (g_task != nullptr) {
        apply(g_picture, &before);
    }
    platform::set_blob_later(kNamespace, kPictureKey, &g_picture, sizeof(g_picture));
}

Stream stream()
{
    std::lock_guard<std::mutex> lock(g_settings_mutex);
    return g_stream;
}

void set_stream(const Stream &stream)
{
    std::lock_guard<std::mutex> lock(g_settings_mutex);
    g_stream = stream;
    g_stream.username[kLoginSize - 1] = 0;
    g_stream.password[kLoginSize - 1] = 0;
    g_stream.enabled = stream.enabled && g_stream.username[0] != 0 && g_stream.password[0] != 0;
    g_stream.max_fps = std::clamp<uint8_t>(stream.max_fps, 1, 30);
    g_stream.quality = std::clamp<uint8_t>(stream.quality, 30, 95);
    detail::login_changed(g_stream);
    platform::Settings settings(kNamespace, platform::Settings::Mode::ReadWrite);
    if (!settings.ok()) {
        return;
    }
    const StoredStream stored{.enabled = g_stream.enabled,
                              .half = g_stream.half,
                              .max_fps = g_stream.max_fps,
                              .quality = g_stream.quality};
    settings.set_blob(kStreamKey, &stored, sizeof(stored));
    settings.set_string(kUserKey, g_stream.username);
    settings.set_string(kPasswordKey, g_stream.password);
    settings.commit();
}

void acquire()
{
    if (g_task != nullptr && g_users.fetch_add(1) == 0) {
        xTaskNotifyGive(g_task);
    }
}

void release()
{
    if (g_task != nullptr) {
        g_users--;
    }
}

bool use_frame(uint32_t &generation, uint32_t timeout_ms, FrameUse use, void *context)
{
    std::unique_lock<std::mutex> lock(g_mutex);
    if (!g_new_frame.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                              [&] { return g_current >= 0 && g_generation != generation; })) {
        return false;
    }
    generation = g_generation;
    use(reinterpret_cast<const uint16_t *>(g_buffers[g_current]), g_width, g_height, context);
    return true;
}

namespace {

int cmd_camreg(int argc, char **argv)
{
    if (argc < 2 || g_fd < 0) {
        std::printf("camreg <reg> [<value>]   (hex)\n");
        return 1;
    }
    esp_cam_sensor_reg_val_t reg{};
    reg.regaddr = static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 16));
    const bool write = argc > 2;
    if (write) {
        reg.value = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 16));
    }
    v4l2_ext_control control{};
    control.id = write ? ESP_CAM_SENSOR_IOC_S_REG : ESP_CAM_SENSOR_IOC_G_REG;
    control.p_u8 = reinterpret_cast<uint8_t *>(&reg);
    control.size = sizeof(reg);
    v4l2_ext_controls controls{};
    controls.ctrl_class = V4L2_CTRL_CLASS_ESP_CAM_IOCTL;
    controls.count = 1;
    controls.controls = &control;
    if (ioctl(g_fd, write ? VIDIOC_S_EXT_CTRLS : VIDIOC_G_EXT_CTRLS, &controls) != 0) {
        std::printf("failed\n");
        return 1;
    }
    std::printf("0x%04" PRIx32 " = 0x%02" PRIx32 "\n", reg.regaddr, reg.value);
    return 0;
}

} // namespace

void register_console()
{
    esp_console_cmd_t command{};
    command.command = "camreg";
    command.help = "camera sensor register: read, or write a value (hex, developer)";
    command.hint = "<reg> [<value>]";
    command.func = cmd_camreg;
    esp_console_cmd_register(&command);

    esp_console_cmd_t settings{};
    settings.command = "camera";
    settings.help = "camera picture: show it, or mirror or flip it (as Settings, Camera, Preview)";
    settings.hint = "[mirror|flip on|off]";
    settings.func = [](int argc, char **argv) {
        Picture p = picture();
        if (argc == 3) {
            const bool on = std::strcmp(argv[2], "on") == 0;
            if (std::strcmp(argv[1], "mirror") == 0) {
                p.mirror = on;
            } else if (std::strcmp(argv[1], "flip") == 0) {
                p.flip = on;
            }
            set_picture(p);
        }
        std::printf("mirror %s, flip %s, auto exposure %s, capture %s\n", p.mirror ? "on" : "off",
                    p.flip ? "on" : "off", p.auto_exposure ? "on" : "off", g_capturing ? "on" : "off");
        return 0;
    };
    esp_console_cmd_register(&settings);
}

namespace detail {

uint32_t newest_generation()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_current >= 0 ? g_generation : 0;
}

} // namespace detail

} // namespace hc::camera
