#pragma once

// Sound through the board's I2S amplifier (docs/architecture.md section 9.6), compiled in with
// CONFIG_HC_AUDIO. One task owns the I2S channel and plays one sound at a time: a newer sound
// replaces the one that plays, so quick taps do not queue up clicks. An alarm sound is the
// exception: it plays at full volume whatever the volume is, and only stop_alarm() or another
// alarm sound replaces it. An announcement (play_mp3) is one in between: only an alarm or another
// announcement replaces it, the touch sounds are dropped while it plays. The channel runs only while a sound plays and stops a second after the
// last one, unless Config::keep_running.
//
// The built-in sounds are 16 bit mono PCM at kSampleRate; an MP3 file (CONFIG_HC_AUDIO_MP3) plays
// at its own sample rate, mixed to mono.
//
// The microphone (CONFIG_HC_MIC) is the codec's ADC on the same I2S port, a receive channel next
// to the speaker's at the speaker's rate. It needs Config::keep_running (the clock runs all the
// time). For now only its level is read, while someone listens (mic_listen()).

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "sdkconfig.h"

namespace hc::audio {

constexpr uint32_t kSampleRate = 16000;
// the longest path play_file() and play_alarm() take
constexpr size_t kMaxPath = 64;

// The I2S output (hc::board::Info::audio_*, wired in main)
struct Config {
    int bclk;
    int ws;
    int dout;
    // the master clock of a codec, 256 x the sample rate; -1 without
    int mclk = -1;
    // A codec between I2S and the amplifier (hc::board::AudioCodec), nullptr without:
    // codec_start() whenever the channel starts or changes the rate, the clock running;
    // codec_stop() before the channel stops.
    esp_err_t (*codec_start)(uint32_t sample_rate) = nullptr;
    void (*codec_stop)() = nullptr;
    // The microphone's samples from the codec (CONFIG_HC_MIC), -1 without; its gain
    // (hc::board::AudioCodec::set_mic) and its registers to the log
    int din = -1;
    esp_err_t (*codec_set_mic)(float gain_db, bool auto_gain) = nullptr;
    void (*codec_log_mic)() = nullptr;
    // The channel (and a codec and the amplifier) run from start() on and do not stop between
    // sounds (hc::board::Info::audio_always_on): an amplifier's wake-up swallowed or softened a
    // click (2026-10-09). Only a file at another rate restarts it.
    bool keep_running = false;
};

// Creates the I2S channel and the task. Call once.
esp_err_t start(const Config &config);

// start() succeeded
bool running();

// Percent of full scale on a perceptual curve; 0 mutes every sound except an alarm.
void set_volume(uint8_t percent);
uint8_t volume();

// Plays `count` samples at kSampleRate. They are read while they play, so they have to stay
// (constant data). From any task; returns at once, like everything below.
void play(const int16_t *samples, size_t count);

// A sine tone for testing the speaker (console `sound tone`), up to 5 s.
void play_tone(uint16_t hz, uint16_t ms);

#if CONFIG_HC_AUDIO_TOUCH_SOUND
// The touch sound: a click of 6 ms.
void play_click();
// The tick of a slider that moves: shorter and softer than the click.
void play_tick();
#endif

#if CONFIG_HC_AUDIO_MP3
// An MP3 file at the volume; a file that cannot be read is logged and skipped.
void play_file(const char *path);

// An MP3 in memory at the volume, as an announcement (a sound from Home Assistant). Takes `data`
// (heap_caps_malloc; PSRAM is fine), also when it does not play: freed with heap_caps_free.
void play_mp3(uint8_t *data, size_t size);
#endif

// The alarm: an MP3 file (CONFIG_HC_AUDIO_MP3), or the built-in beeps when `path` is empty or
// nullptr, at full volume. `loop` repeats it until stop_alarm(); without it, it plays once (the
// preview in Settings).
void play_alarm(const char *path, bool loop);
void stop_alarm();
bool alarm_playing();

// the stack of the audio task that was never used, bytes (console `sound`)
size_t stack_unused();

#if CONFIG_HC_MIC
// the highest gain of set_mic()
constexpr float kMicMaxGainDb = 60.0f;
// the level mic_peak_dbfs() gives for silence
constexpr float kMicSilenceDbfs = -96.0f;

// the microphone's channel was created (start() with Config::din)
bool mic_running();

// A fixed gain of 0 to kMicMaxGainDb, or the codec's automatic level control (then `gain_db`
// does not count). Kept until the next call.
void set_mic(float gain_db, bool auto_gain);

// The samples are read while at least one listener is on: on(true) and on(false) in pairs.
void mic_listen(bool on);

// The loudest sample since the last call, dBFS (kMicSilenceDbfs for silence); false while nobody
// listens or no samples came. One reader at a time: a second one sees the rest.
bool mic_peak_dbfs(float &dbfs);

// the codec's microphone registers to the log (console `mic regs`)
void log_mic();

// A test recording that plays right after it (Settings, Sound; console `mic record`): up to
// kMicRecordMaxS at kSampleRate into PSRAM, then played at the volume as an announcement (a tap's
// click does not cut it off). It ends early when the speaker's channel changes the rate.
constexpr uint32_t kMicRecordMaxS = 5;
enum class RecordState : uint8_t { Idle, Recording, Playing };

// false while one records or plays, at volume 0, or without PSRAM for it
bool mic_record_start();
// ends the recording and plays what it has
void mic_record_stop();
// ends the recording without playing it
void mic_record_cancel();
// what the recording does; the time left while it records
RecordState mic_record_state(uint32_t &remaining_ms);
// the samples of the last recording at kSampleRate (console `mic record`), nullptr before one
const int16_t *mic_recording(size_t &count);
#endif

} // namespace hc::audio
