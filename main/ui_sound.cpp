#include "ui_sound.hpp"

#include <cstdio>
#include <cstring>
#include <dirent.h>

#include "hc/audio/audio.hpp"
#include "hc/platform/settings.hpp"
#include "hc/platform/storage.hpp"
#include "hc/vibration/vibration.hpp"

namespace ui_sound {

namespace {

constexpr const char *kUiNamespace = "ui";

#if CONFIG_HC_AUDIO
// The sound settings since 2026-10-09 evening (volume 0 = silent, a switch per touch sound). The
// first layout under "sound" had the same size, {on, volume, touch_sound}: it is read once
// when there is nothing under "sounds".
constexpr const char *kSoundKey = "sounds";
constexpr const char *kFirstSoundKey = "sound";
struct FirstSoundSettings {
    bool on;
    uint8_t volume;
    bool touch_sound;
};

constexpr hc::ui::SoundSettings kSoundDefaults{.volume = 50, .tap_sound = true, .slider_sound = true};
hc::ui::SoundSettings g_sound = kSoundDefaults;
bool g_sound_loaded = false;

const hc::ui::SoundSettings &stored_sound()
{
    if (g_sound_loaded) {
        return g_sound;
    }
    g_sound_loaded = true;
    hc::platform::Settings settings(kUiNamespace, hc::platform::Settings::Mode::Read);
    if (!settings.ok()) {
        return g_sound;
    }
    hc::ui::SoundSettings stored{};
    size_t size = sizeof(stored);
    if (settings.get_blob(kSoundKey, &stored, size) == ESP_OK && size == sizeof(stored) && stored.volume <= 100) {
        g_sound = stored;
        return g_sound;
    }
    FirstSoundSettings first{};
    size = sizeof(first);
    if (settings.get_blob(kFirstSoundKey, &first, size) == ESP_OK && size == sizeof(first) && first.volume <= 100) {
        g_sound = {.volume = first.on ? first.volume : uint8_t{0},
                   .tap_sound = first.touch_sound,
                   .slider_sound = first.touch_sound};
    }
    return g_sound;
}

bool get_sound(hc::ui::SoundSettings &out)
{
    out = stored_sound();
    return hc::audio::running();
}

void set_sound(const hc::ui::SoundSettings &settings)
{
    g_sound = settings;
    g_sound_loaded = true;
    hc::audio::set_volume(g_sound.volume);
    hc::platform::set_blob_later(kUiNamespace, kSoundKey, &g_sound, sizeof(g_sound));
}
#endif

#if CONFIG_HC_MIC
// The microphone's gain or automatic level control. Speech near the panel came out at about
// -20 dBFS with 42 dB (to be checked on the JC4880P443C).
constexpr const char *kMicKey = "mic";
constexpr hc::ui::MicSettings kMicDefaults{.gain_db = 42, .auto_gain = true};
hc::ui::MicSettings g_mic = kMicDefaults;
bool g_mic_loaded = false;

const hc::ui::MicSettings &stored_mic()
{
    if (!g_mic_loaded) {
        g_mic_loaded = true;
        hc::platform::Settings settings(kUiNamespace, hc::platform::Settings::Mode::Read);
        hc::ui::MicSettings stored{};
        size_t size = sizeof(stored);
        if (settings.ok() && settings.get_blob(kMicKey, &stored, size) == ESP_OK && size == sizeof(stored) &&
            stored.gain_db <= hc::ui::kMicMaxGainDb) {
            g_mic = stored;
        }
    }
    return g_mic;
}

bool get_mic(hc::ui::MicSettings &out)
{
    out = stored_mic();
    return hc::audio::mic_running();
}

void set_mic(const hc::ui::MicSettings &settings)
{
    g_mic = settings;
    g_mic_loaded = true;
    hc::audio::set_mic(g_mic.gain_db, g_mic.auto_gain);
    hc::platform::set_blob_later(kUiNamespace, kMicKey, &g_mic, sizeof(g_mic));
}
#endif

#if CONFIG_HC_VIBRATION
// the Vibration switch, on until it is turned off
constexpr const char *kVibrationKey = "vibrate";
bool g_vibration = true;
bool g_vibration_loaded = false;

bool vibration_on()
{
    if (!g_vibration_loaded) {
        g_vibration_loaded = true;
        hc::platform::Settings settings(kUiNamespace, hc::platform::Settings::Mode::Read);
        uint8_t stored = 0;
        size_t size = sizeof(stored);
        if (settings.ok() && settings.get_blob(kVibrationKey, &stored, size) == ESP_OK && size == sizeof(stored)) {
            g_vibration = stored != 0;
        }
    }
    return g_vibration;
}

bool get_vibration(bool &on)
{
    on = vibration_on();
    return hc::vibration::running();
}

void set_vibration(bool on)
{
    g_vibration = on;
    g_vibration_loaded = true;
    const uint8_t stored = on ? 1 : 0;
    hc::platform::set_blob_later(kUiNamespace, kVibrationKey, &stored, sizeof(stored));
}
#endif

#if CONFIG_HC_AUDIO_ALARM
// The alarms since 2026-10-21 (up to five, a Vibrate switch each). The single alarm before them
// under "alarm" is read once when there is nothing under "alarms": it becomes the first one.
constexpr const char *kAlarmsKey = "alarms";
constexpr const char *kFirstAlarmKey = "alarm";
constexpr const char *kAlarmDir = "/sounds/alarms";
constexpr const char *kAlarmExtension = ".mp3";

struct FirstAlarmSettings {
    bool on;
    uint8_t hour;
    uint8_t minute;
    uint8_t days;
    uint8_t snooze_min;
    char sound[hc::ui::kSoundNameSize];
};

hc::ui::Alarms g_alarms{};
bool g_alarms_loaded = false;

bool valid(const hc::ui::AlarmSettings &s)
{
    return s.hour < 24 && s.minute < 60 && s.days < 0x80 && s.snooze_min >= 1 && s.snooze_min <= 60 &&
           std::memchr(s.sound, 0, sizeof(s.sound)) != nullptr;
}

bool valid(const hc::ui::Alarms &list)
{
    if (list.count > hc::ui::kMaxAlarms) {
        return false;
    }
    for (size_t i = 0; i < list.count; i++) {
        if (!valid(list.list[i])) {
            return false;
        }
    }
    return true;
}

void load_alarms()
{
    hc::platform::Settings settings(kUiNamespace, hc::platform::Settings::Mode::Read);
    if (!settings.ok()) {
        return;
    }
    hc::ui::Alarms stored{};
    size_t size = sizeof(stored);
    if (settings.get_blob(kAlarmsKey, &stored, size) == ESP_OK) {
        if (size == sizeof(stored) && valid(stored)) {
            g_alarms = stored;
        }
        return;
    }
    FirstAlarmSettings first{};
    size = sizeof(first);
    if (settings.get_blob(kFirstAlarmKey, &first, size) != ESP_OK || size != sizeof(first)) {
        return;
    }
    hc::ui::AlarmSettings alarm{.on = first.on,
                                .hour = first.hour,
                                .minute = first.minute,
                                .days = first.days,
                                .snooze_min = first.snooze_min,
                                .sound = {},
                                .vibrate = true};
    std::memcpy(alarm.sound, first.sound, sizeof(alarm.sound));
#if CONFIG_HC_VIBRATION
    // the switch of Settings, Sound decided for the alarm then
    alarm.vibrate = vibration_on();
#endif
    if (valid(alarm)) {
        g_alarms.list[0] = alarm;
        g_alarms.count = 1;
    }
}

void get_alarms(hc::ui::Alarms &out)
{
    if (!g_alarms_loaded) {
        g_alarms_loaded = true;
        load_alarms();
    }
    out = g_alarms;
}

// at once: an alarm set right before a restart has to ring
void store_alarms(const hc::ui::Alarms &list)
{
    g_alarms = list;
    g_alarms_loaded = true;
    hc::platform::Settings stored(kUiNamespace, hc::platform::Settings::Mode::ReadWrite);
    if (stored.ok() && stored.set_blob(kAlarmsKey, &g_alarms, sizeof(g_alarms)) == ESP_OK) {
        stored.commit();
    }
}

// the MP3 files of /fs/sounds/alarms by name, sorted (none without CONFIG_HC_AUDIO_MP3)
size_t list_alarm_sounds(char (*names)[hc::ui::kSoundNameSize], size_t max)
{
#if CONFIG_HC_AUDIO_MP3
    char path[64];
    std::snprintf(path, sizeof(path), "%s%s", hc::platform::kFsMountPoint, kAlarmDir);
    DIR *dir = opendir(path);
    if (dir == nullptr) {
        return 0;
    }
    size_t count = 0;
    const size_t extension = std::strlen(kAlarmExtension);
    while (const dirent *entry = readdir(dir)) {
        const size_t length = std::strlen(entry->d_name);
        if (count == max || length <= extension || length - extension >= hc::ui::kSoundNameSize ||
            std::strcmp(entry->d_name + length - extension, kAlarmExtension) != 0) {
            continue;
        }
        std::snprintf(names[count], hc::ui::kSoundNameSize, "%.*s", static_cast<int>(length - extension),
                      entry->d_name);
        count++;
    }
    closedir(dir);
    // a handful of names: insertion sort (std::sort cannot swap arrays)
    for (size_t i = 1; i < count; i++) {
        char name[hc::ui::kSoundNameSize];
        std::memcpy(name, names[i], sizeof(name));
        size_t j = i;
        for (; j > 0 && std::strcmp(names[j - 1], name) > 0; j--) {
            std::memcpy(names[j], names[j - 1], sizeof(name));
        }
        std::memcpy(names[j], name, sizeof(name));
    }
    return count;
#else
    (void)names;
    (void)max;
    return 0;
#endif
}

void ring(const char *sound, bool loop, bool vibrate)
{
    char path[hc::audio::kMaxPath] = {};
    if (sound != nullptr && sound[0] != 0) {
        std::snprintf(path, sizeof(path), "%s%s/%s%s", hc::platform::kFsMountPoint, kAlarmDir, sound,
                      kAlarmExtension);
    }
    hc::audio::play_alarm(path, loop);
#if CONFIG_HC_VIBRATION
    if (vibrate) {
        // the preview of Settings plays once: the motor follows its sound
        hc::vibration::start_alarm(loop ? nullptr : hc::audio::alarm_playing);
    } else {
        // a preview of an alarm with the switch off ends the pattern of the one before
        hc::vibration::stop_alarm();
    }
#else
    (void)vibrate;
#endif
}

void silence()
{
    hc::audio::stop_alarm();
#if CONFIG_HC_VIBRATION
    hc::vibration::stop_alarm();
#endif
}
#endif

} // namespace

#if CONFIG_HC_AUDIO
bool (*const sound_settings)(hc::ui::SoundSettings &) = get_sound;
void (*const set_sound_settings)(const hc::ui::SoundSettings &) = set_sound;

void apply_stored_volume()
{
    hc::audio::set_volume(stored_sound().volume);
}
#else
bool (*const sound_settings)(hc::ui::SoundSettings &) = nullptr;
void (*const set_sound_settings)(const hc::ui::SoundSettings &) = nullptr;
#endif

#if CONFIG_HC_MIC
bool (*const mic_settings)(hc::ui::MicSettings &) = get_mic;
void (*const set_mic_settings)(const hc::ui::MicSettings &) = set_mic;
void (*const mic_listen)(bool) = hc::audio::mic_listen;
bool (*const mic_level)(float &) = hc::audio::mic_peak_dbfs;
bool (*const mic_record)(bool) = [](bool start) {
    if (start) {
        return hc::audio::mic_record_start();
    }
    hc::audio::mic_record_stop();
    return true;
};
void (*const mic_record_cancel)() = hc::audio::mic_record_cancel;
hc::ui::MicRecording (*const mic_recording)(uint32_t &) = [](uint32_t &remaining_ms) {
    switch (hc::audio::mic_record_state(remaining_ms)) {
    case hc::audio::RecordState::Recording:
        return hc::ui::MicRecording::Recording;
    case hc::audio::RecordState::Playing:
        return hc::ui::MicRecording::Playing;
    case hc::audio::RecordState::Idle:
        break;
    }
    return hc::ui::MicRecording::Idle;
};

void apply_stored_mic()
{
    hc::audio::set_mic(stored_mic().gain_db, stored_mic().auto_gain);
}
#else
bool (*const mic_settings)(hc::ui::MicSettings &) = nullptr;
void (*const set_mic_settings)(const hc::ui::MicSettings &) = nullptr;
void (*const mic_listen)(bool) = nullptr;
bool (*const mic_level)(float &) = nullptr;
bool (*const mic_record)(bool) = nullptr;
void (*const mic_record_cancel)() = nullptr;
hc::ui::MicRecording (*const mic_recording)(uint32_t &) = nullptr;
#endif

#if CONFIG_HC_AUDIO_TOUCH_SOUND
void (*const play_touch_sound)() = hc::audio::play_click;
void (*const play_slider_tick)() = hc::audio::play_tick;
#else
void (*const play_touch_sound)() = nullptr;
void (*const play_slider_tick)() = nullptr;
#endif

#if CONFIG_HC_VIBRATION
bool (*const vibration)(bool &) = get_vibration;
void (*const set_vibration_setting)(bool) = set_vibration;
void (*const vibrate_click)() = hc::vibration::click;
#else
bool (*const vibration)(bool &) = nullptr;
void (*const set_vibration_setting)(bool) = nullptr;
void (*const vibrate_click)() = nullptr;
#endif

#if CONFIG_HC_AUDIO_ALARM
void (*const alarms)(hc::ui::Alarms &) = get_alarms;
void (*const set_alarms)(const hc::ui::Alarms &) = store_alarms;
size_t (*const alarm_sounds)(char (*)[hc::ui::kSoundNameSize], size_t) = list_alarm_sounds;
void (*const play_alarm)(const char *, bool, bool) = ring;
void (*const stop_alarm)() = silence;
#else
void (*const alarms)(hc::ui::Alarms &) = nullptr;
void (*const set_alarms)(const hc::ui::Alarms &) = nullptr;
size_t (*const alarm_sounds)(char (*)[hc::ui::kSoundNameSize], size_t) = nullptr;
void (*const play_alarm)(const char *, bool, bool) = nullptr;
void (*const stop_alarm)() = nullptr;
#endif

} // namespace ui_sound
