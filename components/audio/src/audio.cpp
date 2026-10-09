#include "hc/audio/audio.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "source.hpp"
#if CONFIG_HC_AUDIO_MP3
#include "file_source.hpp"
#endif

namespace hc::audio {

namespace {

using detail::Source;

constexpr const char *kTag = "audio";

// Three DMA buffers of 128 frames: 8 ms each at 16 kHz, so a click starts within about 16 ms of
// play(); 768 bytes of internal RAM.
constexpr uint32_t kDmaFrames = 128;
constexpr uint32_t kDmaBuffers = 3;
// samples written per i2s_channel_write, one DMA buffer: between two of them a newer sound takes
// over
constexpr size_t kChunk = kDmaFrames;
// the channel stops this long after the last sound, so a tap after a tap does not start it again
constexpr TickType_t kIdleStop = pdMS_TO_TICKS(1000);
constexpr uint16_t kMaxToneMs = 5000;
// Silence after the channel starts, before the first sound: the amplifier wakes with the clock
// (the NS4168 when it sees LRCLK, the P4's when the codec switches it on) and swallowed the start
// of a click that came first: quick taps clicked louder than single ones (2026-10-09).
constexpr uint32_t kWakeMs = 20;
constexpr size_t kQueueLength = 4;
#if CONFIG_HC_AUDIO_MP3
constexpr bool kFiles = true;
#else
constexpr bool kFiles = false;
#endif

// The MP3 decoder runs on this task, and so do the file reads: its stack has to be internal RAM
// (the flash cache is off while LittleFS reads). Decoding used 2.1 KB of it (2026-10-09, `sound`
// shows what was never used).
#if CONFIG_HC_AUDIO_MP3
constexpr uint32_t kStackSize = 5 * 1024;
#else
constexpr uint32_t kStackSize = 3072;
#endif
// above the LVGL task (4), so a busy screen does not starve the DMA buffers
constexpr UBaseType_t kPriority = 5;

enum class Kind : uint8_t { Samples, Tone, Beeps, File, StopAlarm };

struct Request {
    Kind kind;
    bool alarm; // full volume, only another alarm or StopAlarm replaces it
    bool loop;
    const int16_t *samples;
    uint32_t count;
    uint16_t hz;
    char path[kMaxPath];
};

i2s_chan_handle_t g_channel = nullptr;
Config g_config{};
QueueHandle_t g_queue = nullptr;
TaskHandle_t g_task = nullptr;
std::atomic<uint8_t> g_volume{50};
// the volume as a factor in Q15 (32768 = full scale)
std::atomic<int32_t> g_gain{0};
std::atomic<bool> g_alarm{false};

int32_t gain_of(uint8_t percent)
{
    // square of the percentage: the loudness the ear hears grows about evenly with the slider
    const int32_t p = std::min<int32_t>(percent, 100);
    return p * p * 32768 / 10000;
}

// The task's state: the sound that plays and the channel
class Player {
public:
    // a request from the queue: it replaces the sound that plays, except a normal sound over an
    // alarm
    void accept(const Request &request)
    {
        if (request.kind == Kind::StopAlarm) {
            if (alarm_) {
                write_silence((kDmaFrames - fill_) % kDmaFrames);
                idle_ = true;
                finish();
            }
            return;
        }
        if (alarm_ && !request.alarm) {
            return;
        }
        finish();
        active_ = open(request);
        if (active_ != nullptr) {
            alarm_ = request.alarm;
            loop_ = request.loop;
            g_alarm.store(alarm_);
        }
    }

    bool playing() const
    {
        return active_ != nullptr;
    }

    // the next samples to the channel; at the end the sound loops or stops
    void step(int16_t *buffer)
    {
        size_t count = active_->read(buffer, kChunk);
        if (count == 0 && loop_ && active_->restart()) {
            count = active_->read(buffer, kChunk);
        }
        if (count == 0) {
            // the end: the rest of its DMA buffer silent, the next sound starts in a buffer of
            // its own (see lead_in())
            write_silence((kDmaFrames - fill_) % kDmaFrames);
            idle_ = true;
            finish();
            return;
        }
        if (!prepare(active_->sample_rate())) {
            finish();
            return;
        }
        if (idle_) {
            lead_in();
            idle_ = false;
        }
        const int32_t gain = alarm_ ? 32768 : g_gain.load(std::memory_order_relaxed);
        for (size_t i = 0; i < count; i++) {
            buffer[i] = static_cast<int16_t>(std::clamp<int32_t>((buffer[i] * gain) >> 15, INT16_MIN, INT16_MAX));
        }
        write(buffer, count);
    }

    void stop_channel()
    {
        if (enabled_) {
            if (g_config.codec_stop != nullptr) {
                g_config.codec_stop();
            }
            i2s_channel_disable(g_channel);
            enabled_ = false;
            // the driver starts over with its first buffer
            fill_ = 0;
            idle_ = true;
        }
    }

    bool channel_running() const
    {
        return enabled_;
    }

    // the channel on at `rate` without a sound (Config::keep_running)
    void run(uint32_t rate)
    {
        prepare(rate);
    }

private:
    Source *open(const Request &request)
    {
        switch (request.kind) {
        case Kind::Samples:
            memory_.open(request.samples, request.count);
            return &memory_;
        case Kind::Tone:
            tone_.open(request.hz, request.count);
            return &tone_;
        case Kind::Beeps:
            beeps_.open();
            return &beeps_;
        case Kind::File:
#if CONFIG_HC_AUDIO_MP3
            if (file_.open(request.path)) {
                return &file_;
            }
            // an alarm rings anyway
            if (request.alarm) {
                beeps_.open();
                return &beeps_;
            }
#endif
            return nullptr;
        case Kind::StopAlarm:
            break;
        }
        return nullptr;
    }

    void finish()
    {
#if CONFIG_HC_AUDIO_MP3
        if (active_ == &file_) {
            file_.close();
        }
#endif
        active_ = nullptr;
        alarm_ = false;
        g_alarm.store(false);
    }

    void write(const int16_t *samples, size_t count)
    {
        size_t written = 0;
        i2s_channel_write(g_channel, samples, count * sizeof(int16_t), &written, portMAX_DELAY);
        fill_ = static_cast<uint32_t>((fill_ + count) % kDmaFrames);
    }

    void write_silence(uint32_t samples)
    {
        static const int16_t zeros[kChunk] = {};
        while (samples > 0) {
            const uint32_t count = std::min<uint32_t>(samples, kChunk);
            write(zeros, count);
            samples -= count;
        }
    }

    // Before the first sound after a pause: a DMA buffer of silence. After a pause every buffer
    // is back with the driver, and the first one it hands out is the one the DMA is sending right
    // now (i2s_common.c takes a buffer when its queue is almost full): a sound written there lost
    // what the DMA had already passed, so clicks came out shorter and softer at random
    // (2026-10-09). The silence goes there; the sound lands in the buffer after it.
    void lead_in()
    {
        write_silence(kDmaFrames);
    }

    // the channel runs at `rate`: a file has a rate of its own, the built-in sounds 16 kHz
    bool prepare(uint32_t rate)
    {
        if (rate != rate_) {
            stop_channel();
            const i2s_std_clk_config_t clock = I2S_STD_CLK_DEFAULT_CONFIG(rate);
            if (i2s_channel_reconfig_std_clock(g_channel, &clock) != ESP_OK) {
                ESP_LOGW(kTag, "no I2S clock for %lu Hz", static_cast<unsigned long>(rate));
                return false;
            }
            rate_ = rate;
        }
        if (!enabled_) {
            // the DMA sends zeros between sounds (auto_clear), so the channel can keep running
            enabled_ = i2s_channel_enable(g_channel) == ESP_OK;
            if (!enabled_) {
                ESP_LOGW(kTag, "I2S channel did not start");
            } else if (g_config.codec_start != nullptr && g_config.codec_start(rate) != ESP_OK) {
                i2s_channel_disable(g_channel);
                enabled_ = false;
            } else {
                // whole buffers, so the sounds start at the beginning of one
                const uint32_t wake = rate * kWakeMs / 1000;
                write_silence((wake + kDmaFrames - 1) / kDmaFrames * kDmaFrames);
            }
        }
        return enabled_;
    }

    detail::MemorySource memory_;
    detail::ToneSource tone_;
    detail::BeepsSource beeps_;
#if CONFIG_HC_AUDIO_MP3
    detail::FileSource file_;
#endif
    Source *active_ = nullptr;
    bool alarm_ = false;
    bool loop_ = false;
    // samples in the DMA buffer being filled
    uint32_t fill_ = 0;
    // no sound since the last one ended: the next one needs a lead-in
    bool idle_ = true;
    bool enabled_ = false;
    uint32_t rate_ = kSampleRate;
};

void audio_task(void *)
{
    static Player player;
    static int16_t buffer[kChunk];
    if (g_config.keep_running) {
        player.run(kSampleRate);
    }
    for (;;) {
        // playing: only look for a newer sound; idle: wait for one, the channel stops meanwhile
        // (or runs on, Config::keep_running)
        const TickType_t idle = g_config.keep_running ? portMAX_DELAY : kIdleStop;
        const TickType_t wait = player.playing() ? 0 : (player.channel_running() ? idle : portMAX_DELAY);
        Request request;
        if (xQueueReceive(g_queue, &request, wait) == pdTRUE) {
            do {
                player.accept(request);
            } while (xQueueReceive(g_queue, &request, 0) == pdTRUE);
        } else if (!player.playing()) {
            player.stop_channel();
            continue;
        }
        if (player.playing()) {
            player.step(buffer);
        }
    }
}

void post(const Request &request)
{
    if (g_queue == nullptr) {
        return;
    }
    if (!request.alarm && request.kind != Kind::StopAlarm && g_volume.load(std::memory_order_relaxed) == 0) {
        return;
    }
    // a full queue (four sounds within one chunk) drops this one
    xQueueSend(g_queue, &request, 0);
}

Request make(Kind kind)
{
    Request request{};
    request.kind = kind;
    return request;
}

} // namespace

esp_err_t start(const Config &config)
{
    ESP_RETURN_ON_FALSE(g_channel == nullptr, ESP_ERR_INVALID_STATE, kTag, "started twice");
    // read by the task, which starts below
    g_config = config;

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = kDmaBuffers;
    channel_config.dma_frame_num = kDmaFrames;
    // silence when no sound is written, instead of the last buffer over and over
    channel_config.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, &g_channel, nullptr), kTag, "I2S channel");

    i2s_std_config_t std_config{};
    std_config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
    std_config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    // the mono samples in both slots: the amplifier plays whichever channel its CTRL pin selects
    std_config.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    // a codec needs the master clock (I2S_STD_CLK_DEFAULT_CONFIG: 256 x the sample rate)
    std_config.gpio_cfg.mclk = config.mclk >= 0 ? static_cast<gpio_num_t>(config.mclk) : I2S_GPIO_UNUSED;
    std_config.gpio_cfg.bclk = static_cast<gpio_num_t>(config.bclk);
    std_config.gpio_cfg.ws = static_cast<gpio_num_t>(config.ws);
    std_config.gpio_cfg.dout = static_cast<gpio_num_t>(config.dout);
    std_config.gpio_cfg.din = I2S_GPIO_UNUSED;
    esp_err_t err = i2s_channel_init_std_mode(g_channel, &std_config);
#if CONFIG_HC_AUDIO_MP3
    detail::FileSource::init();
#endif
    if (err == ESP_OK) {
        g_queue = xQueueCreate(kQueueLength, sizeof(Request));
        err = g_queue != nullptr && xTaskCreate(audio_task, "audio", kStackSize, nullptr, kPriority, &g_task) == pdPASS
                  ? ESP_OK
                  : ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "start failed: %s", esp_err_to_name(err));
        if (g_queue != nullptr) {
            vQueueDelete(g_queue);
            g_queue = nullptr;
        }
        i2s_del_channel(g_channel);
        g_channel = nullptr;
        return err;
    }
    set_volume(g_volume.load());
    ESP_LOGI(kTag, "I2S out BCLK %d, WS %d, DOUT %d, MCLK %d%s", config.bclk, config.ws, config.dout, config.mclk,
             config.codec_start != nullptr ? ", codec" : "");
    return ESP_OK;
}

bool running()
{
    return g_queue != nullptr;
}

void set_volume(uint8_t percent)
{
    percent = std::min<uint8_t>(percent, 100);
    g_volume.store(percent, std::memory_order_relaxed);
    g_gain.store(gain_of(percent), std::memory_order_relaxed);
}

uint8_t volume()
{
    return g_volume.load(std::memory_order_relaxed);
}

void play(const int16_t *samples, size_t count)
{
    if (samples != nullptr && count > 0) {
        Request request = make(Kind::Samples);
        request.samples = samples;
        request.count = static_cast<uint32_t>(count);
        post(request);
    }
}

void play_tone(uint16_t hz, uint16_t ms)
{
    if (hz > 0 && hz < kSampleRate / 2 && ms > 0) {
        Request request = make(Kind::Tone);
        request.hz = hz;
        request.count = kSampleRate * std::min(ms, kMaxToneMs) / 1000;
        post(request);
    }
}

#if CONFIG_HC_AUDIO_MP3
void play_file(const char *path)
{
    Request request = make(Kind::File);
    std::snprintf(request.path, sizeof(request.path), "%s", path);
    post(request);
}
#endif

void play_alarm(const char *path, bool loop)
{
    const bool file = kFiles && path != nullptr && path[0] != 0;
    Request request = make(file ? Kind::File : Kind::Beeps);
    request.alarm = true;
    request.loop = loop;
    if (file) {
        std::snprintf(request.path, sizeof(request.path), "%s", path);
    }
    post(request);
}

void stop_alarm()
{
    post(make(Kind::StopAlarm));
}

bool alarm_playing()
{
    return g_alarm.load();
}

size_t stack_unused()
{
    return g_task != nullptr ? uxTaskGetStackHighWaterMark(g_task) : 0;
}

} // namespace hc::audio
