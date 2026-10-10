#pragma once

// The Sound, Vibration and Alarm entries of the screens' Backend (hc/ui/backend.hpp), made of the
// audio, vibration and platform components. Each is nullptr when its option is not built in
// (CONFIG_HC_AUDIO, CONFIG_HC_AUDIO_TOUCH_SOUND, CONFIG_HC_VIBRATION, CONFIG_HC_AUDIO_ALARM), so
// the UI leaves the page or switch out. The alarm vibrates along with its sound.
// Wiring only, like the rest of main.

#include <cstddef>

#include "hc/ui/backend.hpp"
#include "sdkconfig.h"

namespace ui_sound {

extern bool (*const sound_settings)(hc::ui::SoundSettings &out);
extern void (*const set_sound_settings)(const hc::ui::SoundSettings &settings);
extern void (*const play_touch_sound)();
extern void (*const play_slider_tick)();

extern bool (*const vibration)(bool &on);
extern void (*const set_vibration_setting)(bool on);
extern void (*const vibrate_click)();

extern void (*const alarm_settings)(hc::ui::AlarmSettings &out);
extern void (*const set_alarm_settings)(const hc::ui::AlarmSettings &settings);
extern size_t (*const alarm_sounds)(char (*names)[hc::ui::kSoundNameSize], size_t max);
extern void (*const play_alarm)(const char *sound, bool loop);
extern void (*const stop_alarm)();

#if CONFIG_HC_AUDIO
// The stored volume to the audio output, after hc::audio::start().
void apply_stored_volume();
#endif

} // namespace ui_sound
