#pragma once

// The Sound, Vibration and Alarm entries of the screens' Backend (hc/ui/backend.hpp), made of the
// audio, vibration and platform components. Each is nullptr when its option is not built in
// (CONFIG_HC_AUDIO, CONFIG_HC_AUDIO_TOUCH_SOUND, CONFIG_HC_MIC, CONFIG_HC_VIBRATION, CONFIG_HC_AUDIO_ALARM), so
// the UI leaves the page or switch out. An alarm with its Vibrate switch on vibrates along with
// its sound. Wiring only, like the rest of main.

#include <cstddef>

#include "hc/ui/backend.hpp"
#include "sdkconfig.h"

namespace ui_sound {

extern bool (*const sound_settings)(hc::ui::SoundSettings &out);
extern void (*const set_sound_settings)(const hc::ui::SoundSettings &settings);
extern void (*const play_touch_sound)();
extern void (*const play_slider_tick)();

extern bool (*const mic_settings)(hc::ui::MicSettings &out);
extern void (*const set_mic_settings)(const hc::ui::MicSettings &settings);
extern void (*const mic_listen)(bool on);
extern bool (*const mic_level)(float &dbfs);
extern bool (*const mic_record)(bool start);
extern void (*const mic_record_cancel)();
extern hc::ui::MicRecording (*const mic_recording)(uint32_t &remaining_ms);

extern bool (*const vibration)(bool &on);
extern void (*const set_vibration_setting)(bool on);
extern void (*const vibrate_click)();

extern void (*const alarms)(hc::ui::Alarms &out);
extern void (*const set_alarms)(const hc::ui::Alarms &alarms);
extern size_t (*const alarm_sounds)(char (*names)[hc::ui::kSoundNameSize], size_t max);
extern void (*const play_alarm)(const char *sound, bool loop, bool vibrate);
extern void (*const stop_alarm)();

#if CONFIG_HC_AUDIO
// The stored volume to the audio output, after hc::audio::start().
void apply_stored_volume();
#endif

#if CONFIG_HC_MIC
// The stored gain to the microphone, after hc::audio::start().
void apply_stored_mic();
#endif

} // namespace ui_sound
