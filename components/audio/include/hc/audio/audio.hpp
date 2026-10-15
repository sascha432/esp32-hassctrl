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

} // namespace hc::audio
