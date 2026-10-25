#pragma once

// Sound through the board's I2S amplifier (docs/architecture.md section 9.6), compiled in with
// CONFIG_HC_AUDIO. One task owns the I2S channel and plays one sound at a time: a newer sound
// replaces the one that plays, so quick taps do not queue up clicks. An alarm sound is the
// exception: it plays at full volume whatever the volume is, and only stop_alarm() or another
// alarm sound replaces it. An announcement (play_mp3) is one in between: only an alarm or another
// announcement replaces it, the touch sounds are dropped while it plays. The music of the player
// (play_media, play_stream) has a place of its own: an alarm or an announcement interrupts it and
// it goes on where it was when that ended; the touch sounds are dropped while there is music. The
// channel runs only while a sound plays and stops a second after the last one, unless
// Config::keep_running.
//
// The built-in sounds are 16 bit mono PCM at kSampleRate; an MP3 file (CONFIG_HC_AUDIO_MP3) plays
// at its own sample rate, mixed to mono.
//
// The microphone (CONFIG_HC_MIC) is the codec's ADC on the same I2S port, a receive channel next
// to the speaker's at the speaker's rate. It needs Config::keep_running (the clock runs all the
// time). For now only its level is read, while someone listens (mic_listen()).

#include <cstddef>
#include <cstdint>
#include <memory>

#include "esp_err.h"
#include "sdkconfig.h"

#if CONFIG_HC_UI_PLAYER
#include "hc/core/byte_ring.hpp"
#include "hc/formats/mp3_tags.hpp"
#endif

namespace hc::audio {

constexpr uint32_t kSampleRate = 16000;
// the longest path play_file(), play_alarm() and play_media() take (an SD card's)
constexpr size_t kMaxPath = 128;

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

// The music of the player (components/player, CONFIG_HC_UI_PLAYER): a WAV file (16 bit PCM) or an
// MP3 (CONFIG_HC_AUDIO_MP3) at the volume, with pause, seek and a spectrum of what plays. false
// when it cannot start (volume 0, no audio); a file that cannot be read ends as MediaState::Failed.
// Buffering: a download has not enough to play yet (at the start, after a seek, when it ran dry);
// Interrupted: an alarm or an announcement plays, the music goes on after it.
enum class MediaState : uint8_t { Idle, Playing, Paused, Failed, Buffering, Interrupted };

struct MediaStatus {
    MediaState state;
    uint32_t position_ms;
    // 0 while it is not known (an MP3 before its first frame)
    uint32_t duration_ms;
};

#if CONFIG_HC_UI_PLAYER
bool play_media(const char *path);
// A download on its way (an MP3 of Home Assistant's media library): it plays once a part of it is
// there. The task reads `ring` and cancels it when the music ends or is replaced, which ends the
// download; a seek asks the download for the bytes from there.
bool play_stream(std::shared_ptr<core::ByteRing> ring);
void pause_media(bool pause);
void seek_media(uint32_t ms);
void stop_media();
MediaStatus media_status();

// Called on the audio task when the music ended by itself (`failed`: it could not be read, or its
// download broke), not when it was stopped or replaced: the player's queue goes on. Quick: post
// and return.
using MediaEnded = void (*)(bool failed);
void set_media_ended(MediaEnded callback);

// The tags of the MP3 that plays (title, artist, album, cover), null before they were read; a WAV
// has none. `serial` changes with every file's tags.
std::shared_ptr<const formats::Mp3Tags> media_tags(uint32_t &serial);

// For the console: the rate the channel runs at, and how often its DMA ran dry while music
// played (since start).
struct MediaDebug {
    uint32_t rate;
    uint32_t underruns;
};
MediaDebug media_debug();

// The levels of what the player plays now in `count` bands spaced evenly in octaves from 40 Hz
// to half the rate (at least one FFT bin each, so the lowest step 43 Hz at a time), 0 (-60 dB and
// below) to 1 (full scale), before the volume. A 1024 point FFT on the caller's task (the
// screen's, about 30 times a second); false while nothing plays.
bool media_spectrum(float *bands, size_t count);
#endif

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
