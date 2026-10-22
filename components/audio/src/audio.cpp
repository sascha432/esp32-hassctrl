#include "hc/audio/audio.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "source.hpp"
#if CONFIG_HC_AUDIO_MP3
#include "mp3_source.hpp"
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

enum class Kind : uint8_t { Samples, Tone, Beeps, File, Buffer, StopAlarm };

struct Request {
    Kind kind;
    bool alarm; // full volume, only another alarm or StopAlarm replaces it
    // a sound from Home Assistant: only an alarm or another announcement replaces it
    bool announcement;
    bool loop;
    const int16_t *samples;
    uint32_t count;
    // Buffer: an MP3 the request owns (play_mp3)
    uint8_t *data;
    size_t size;
    uint16_t hz;
    char path[kMaxPath];
};

i2s_chan_handle_t g_channel = nullptr;
// the microphone's receive channel (CONFIG_HC_MIC), nullptr without
i2s_chan_handle_t g_rx = nullptr;
Config g_config{};
QueueHandle_t g_queue = nullptr;
TaskHandle_t g_task = nullptr;
std::atomic<uint8_t> g_volume{50};
// the volume as a factor in Q15 (32768 = full scale)
std::atomic<int32_t> g_gain{0};
std::atomic<bool> g_alarm{false};

#if CONFIG_HC_MIC
// The test recording (mic_record_start), PSRAM, kept once allocated: it plays from there. Posted:
// on its way to the task; plays: the task plays it now.
int16_t *g_record = nullptr;
std::atomic<bool> g_record_posted{false};
std::atomic<bool> g_record_plays{false};
// the channel's rate (Player::prepare): a recording is at kSampleRate
std::atomic<uint32_t> g_rate{kSampleRate};
#endif

int32_t gain_of(uint8_t percent)
{
    // square of the percentage: the loudness the ear hears grows about evenly with the slider
    const int32_t p = std::min<int32_t>(percent, 100);
    return p * p * 32768 / 10000;
}

// a request that does not play: the buffer it owns goes
void drop(const Request &request)
{
    if (request.kind == Kind::Buffer) {
        heap_caps_free(request.data);
    }
}

// The task's state: the sound that plays and the channel
class Player {
public:
    // a request from the queue: it replaces the sound that plays, except a normal sound over an
    // alarm or an announcement, and an announcement over an alarm
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
#if CONFIG_HC_MIC
        const bool recording = request.kind == Kind::Samples && g_record != nullptr && request.samples == g_record;
        if (recording) {
            g_record_posted.store(false);
        }
#endif
        if ((alarm_ && !request.alarm) || (announcement_ && !request.alarm && !request.announcement)) {
            drop(request);
            return;
        }
        finish();
        active_ = open(request);
        if (active_ != nullptr) {
            alarm_ = request.alarm;
            announcement_ = request.announcement;
            loop_ = request.loop;
            g_alarm.store(alarm_);
#if CONFIG_HC_MIC
            g_record_plays.store(recording);
#endif
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
            if (g_rx != nullptr) {
                i2s_channel_disable(g_rx);
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
        case Kind::Buffer:
#if CONFIG_HC_AUDIO_MP3
            // the buffer is the source's now
            if (file_.open(request.data, request.size)) {
                return &file_;
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
        announcement_ = false;
        g_alarm.store(false);
#if CONFIG_HC_MIC
        g_record_plays.store(false);
#endif
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
            if (i2s_channel_reconfig_std_clock(g_channel, &clock) != ESP_OK ||
                (g_rx != nullptr && i2s_channel_reconfig_std_clock(g_rx, &clock) != ESP_OK)) {
                ESP_LOGW(kTag, "no I2S clock for %lu Hz", static_cast<unsigned long>(rate));
                return false;
            }
            rate_ = rate;
#if CONFIG_HC_MIC
            g_rate.store(rate);
#endif
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
                // the microphone's samples come with the speaker's clock
                if (g_rx != nullptr && i2s_channel_enable(g_rx) != ESP_OK) {
                    ESP_LOGW(kTag, "I2S microphone channel did not start");
                }
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
    detail::Mp3Source file_;
#endif
    Source *active_ = nullptr;
    bool alarm_ = false;
    bool announcement_ = false;
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

// false when it was dropped
bool post(const Request &request)
{
    if (g_queue == nullptr) {
        return false;
    }
    if (!request.alarm && request.kind != Kind::StopAlarm && g_volume.load(std::memory_order_relaxed) == 0) {
        drop(request);
        return false;
    }
    // a full queue (four sounds within one chunk) drops this one
    if (xQueueSend(g_queue, &request, 0) != pdTRUE) {
        drop(request);
        return false;
    }
    return true;
}

Request make(Kind kind)
{
    Request request{};
    request.kind = kind;
    return request;
}

#if CONFIG_HC_MIC
// 512 samples a read: 32 ms at 16 kHz
constexpr size_t kMicChunk = 512;
constexpr uint32_t kMicStackSize = 2560;
// The audio task's, above LVGL: the receive DMA holds only kDmaBuffers x kDmaFrames (24 ms at
// 16 kHz, the speaker's settings), and a recording must not miss a read while the screen draws.
constexpr UBaseType_t kMicPriority = 5;
constexpr TickType_t kMicReadTimeout = pdMS_TO_TICKS(200);

constexpr size_t kRecordSamples = kSampleRate * kMicRecordMaxS;
// The tap that starts or stops a recording clicks (touch sound) and it reached the recording:
// the first 200 ms are not kept, and on Stop the last 150 ms go (the click comes within about
// 30 ms of the tap, the stop up to one read later).
constexpr size_t kRecordLeadSamples = kSampleRate * 200 / 1000;
constexpr size_t kRecordTailSamples = kSampleRate * 150 / 1000;

TaskHandle_t g_mic_task = nullptr;
std::atomic<int> g_mic_listeners{0};
// the largest sample since the last mic_peak_dbfs(), -1: none read
std::atomic<int32_t> g_mic_peak{-1};

enum class RecordCommand : uint8_t { None, Stop, Cancel };
std::atomic<RecordState> g_record_state{RecordState::Idle};
std::atomic<RecordCommand> g_record_command{RecordCommand::None};
std::atomic<size_t> g_record_count{0};
// samples still to skip at the start of a recording (kRecordLeadSamples)
std::atomic<size_t> g_record_skip{0};

// on the mic task: the recording plays (unless `play` is false or it is empty), or is gone;
// `stopped`: ended by a tap, whose click is in its last samples
void end_recording(bool play, bool stopped)
{
    size_t count = g_record_count.load();
    count -= stopped ? std::min(count, kRecordTailSamples) : 0;
    if (play && count > 0) {
        Request request = make(Kind::Samples);
        request.announcement = true;
        request.samples = g_record;
        request.count = static_cast<uint32_t>(count);
        g_record_posted.store(true);
        if (post(request)) {
            g_record_state.store(RecordState::Playing);
            return;
        }
        g_record_posted.store(false);
    }
    g_record_state.store(RecordState::Idle);
}

// Reads while someone listens or records and keeps the largest sample, and the samples of a
// recording; waits otherwise. The channel is off while the speaker's changes the rate: the read
// fails then and waits a little (a recording ends there, its rate is gone).
void mic_task(void *)
{
    static int16_t samples[kMicChunk];
    for (;;) {
        bool recording = g_record_state.load() == RecordState::Recording;
        if (recording) {
            const RecordCommand command = g_record_command.exchange(RecordCommand::None);
            if (command != RecordCommand::None || g_rate.load() != kSampleRate) {
                end_recording(command != RecordCommand::Cancel, command == RecordCommand::Stop);
                continue;
            }
        }
        if (g_mic_listeners.load() == 0 && !recording) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        size_t bytes = 0;
        if (i2s_channel_read(g_rx, samples, sizeof(samples), &bytes, kMicReadTimeout) != ESP_OK || bytes == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        const size_t count = bytes / sizeof(int16_t);
        int32_t peak = 0;
        for (size_t i = 0; i < count; i++) {
            peak = std::max<int32_t>(peak, std::abs(static_cast<int32_t>(samples[i])));
        }
        int32_t seen = g_mic_peak.load();
        while (peak > seen && !g_mic_peak.compare_exchange_weak(seen, peak)) {
        }
        if (recording) {
            const size_t skip = std::min(count, g_record_skip.load());
            g_record_skip.fetch_sub(skip);
            const size_t done = g_record_count.load();
            const size_t take = std::min(count - skip, kRecordSamples - done);
            std::memcpy(g_record + done, samples + skip, take * sizeof(int16_t));
            g_record_count.store(done + take);
            if (done + take == kRecordSamples) {
                end_recording(true, false);
            }
        }
    }
}

// The receive channel next to the speaker's (created with it), the left slot: the codec sends its
// ADC there, the DAC's signal in the right one
esp_err_t start_mic(const i2s_std_config_t &speaker)
{
    i2s_std_config_t config = speaker;
    config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    config.gpio_cfg.din = static_cast<gpio_num_t>(g_config.din);
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(g_rx, &config), kTag, "I2S microphone channel");
    ESP_RETURN_ON_FALSE(xTaskCreate(mic_task, "mic", kMicStackSize, nullptr, kMicPriority, &g_mic_task) == pdPASS,
                        ESP_ERR_NO_MEM, kTag, "mic task");
    return ESP_OK;
}
#endif

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
#if CONFIG_HC_MIC
    // the microphone needs the clock all the time; without it the codec's ADC stays unread
    const bool mic = config.din >= 0 && config.keep_running;
    if (config.din >= 0 && !config.keep_running) {
        ESP_LOGW(kTag, "microphone left out: it needs keep_running");
    }
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, &g_channel, mic ? &g_rx : nullptr), kTag, "I2S channel");
#else
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, &g_channel, nullptr), kTag, "I2S channel");
#endif

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
#if CONFIG_HC_MIC
    if (err == ESP_OK && g_rx != nullptr && start_mic(std_config) != ESP_OK) {
        // the speaker without it
        i2s_del_channel(g_rx);
        g_rx = nullptr;
    }
#endif
#if CONFIG_HC_AUDIO_MP3
    detail::Mp3Source::init();
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
#if CONFIG_HC_MIC
    if (g_rx != nullptr) {
        ESP_LOGI(kTag, "I2S microphone DIN %d", config.din);
    }
#endif
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

void play_mp3(uint8_t *data, size_t size)
{
    if (g_queue == nullptr) {
        heap_caps_free(data);
        return;
    }
    Request request = make(Kind::Buffer);
    request.announcement = true;
    request.data = data;
    request.size = size;
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

#if CONFIG_HC_MIC
bool mic_running()
{
    return g_rx != nullptr;
}

void set_mic(float gain_db, bool auto_gain)
{
    if (g_rx != nullptr && g_config.codec_set_mic != nullptr) {
        g_config.codec_set_mic(std::clamp(gain_db, 0.0f, kMicMaxGainDb), auto_gain);
    }
}

void mic_listen(bool on)
{
    if (g_rx == nullptr) {
        return;
    }
    if (!on) {
        g_mic_listeners.fetch_sub(1);
        return;
    }
    if (g_mic_listeners.fetch_add(1) == 0) {
        g_mic_peak.store(-1);
        xTaskNotifyGive(g_mic_task);
    }
}

bool mic_peak_dbfs(float &dbfs)
{
    const int32_t peak = g_mic_peak.exchange(-1);
    if (g_mic_listeners.load() == 0 || peak < 0) {
        return false;
    }
    dbfs = peak > 0 ? std::max(kMicSilenceDbfs, 20.0f * std::log10(static_cast<float>(peak) / 32768.0f))
                    : kMicSilenceDbfs;
    return true;
}

void log_mic()
{
    if (g_rx != nullptr && g_config.codec_log_mic != nullptr) {
        g_config.codec_log_mic();
    }
}

bool mic_record_start()
{
    uint32_t remaining = 0;
    if (g_rx == nullptr || volume() == 0 || mic_record_state(remaining) != RecordState::Idle) {
        return false;
    }
    if (g_record == nullptr) {
        g_record = static_cast<int16_t *>(heap_caps_malloc(kRecordSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
        if (g_record == nullptr) {
            ESP_LOGW(kTag, "no PSRAM for a recording");
            return false;
        }
    }
    g_record_count.store(0);
    g_record_skip.store(kRecordLeadSamples);
    g_record_command.store(RecordCommand::None);
    g_record_state.store(RecordState::Recording);
    xTaskNotifyGive(g_mic_task);
    return true;
}

void mic_record_stop()
{
    if (g_record_state.load() == RecordState::Recording) {
        g_record_command.store(RecordCommand::Stop);
    }
}

void mic_record_cancel()
{
    if (g_record_state.load() == RecordState::Recording) {
        g_record_command.store(RecordCommand::Cancel);
    }
}

const int16_t *mic_recording(size_t &count)
{
    count = g_record_state.load() == RecordState::Recording ? 0 : g_record_count.load();
    return g_record;
}

RecordState mic_record_state(uint32_t &remaining_ms)
{
    remaining_ms = 0;
    RecordState state = g_record_state.load();
    // played to the end, or replaced by an alarm or an announcement
    if (state == RecordState::Playing && !g_record_posted.load() && !g_record_plays.load()) {
        g_record_state.compare_exchange_strong(state, RecordState::Idle);
        return RecordState::Idle;
    }
    if (state == RecordState::Recording) {
        remaining_ms = static_cast<uint32_t>((kRecordSamples - g_record_count.load()) * 1000 / kSampleRate);
    }
    return state;
}
#endif

} // namespace hc::audio
