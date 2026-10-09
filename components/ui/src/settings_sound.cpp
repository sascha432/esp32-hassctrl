// Settings, Sound (docs/ui/screen.md section 5, Sound): only with an audio output (sound::available()).
// The volume is also on quick settings (volume_card()).

#include "settings.hpp"
#include "settings_pages.hpp"
#include "sound.hpp"
#include "theme.hpp"

namespace hc::ui::settings::pages {

namespace {

namespace t = theme;

SoundSettings sound_settings()
{
    SoundSettings s{};
    backend().sound_settings(s);
    return s;
}

void store(const SoundSettings &s)
{
    backend().set_sound_settings(s);
    sound::settings_changed();
}

void touch_switch(lv_obj_t *parent, const char *name, bool on, lv_event_cb_t callback)
{
    lv_obj_t *row = t::row(parent, name, "", false, nullptr);
    t::toggle(row, on, callback);
}

} // namespace

void volume_card(lv_obj_t *parent)
{
    lv_obj_t *slider = slider_card(parent, "Volume", 0, 100, sound_settings().volume, [](lv_event_t *event) {
        SoundSettings changed = sound_settings();
        changed.volume = static_cast<uint8_t>(lv_slider_get_value(lv_event_get_target(event)));
        store(changed);
    });
    // the new volume to hear: the click when the slider is let go
    lv_obj_add_event_cb(
        slider,
        [](lv_event_t *) {
            if (backend().play_touch_sound != nullptr && sound_settings().volume > 0) {
                backend().play_touch_sound();
            }
        },
        LV_EVENT_RELEASED, nullptr);
}

Built sound_page(lv_obj_t *content)
{
    lv_obj_t *col = t::column(content);
    const SoundSettings s = sound_settings();
    volume_card(col);

    if (backend().play_touch_sound != nullptr) {
        section(col, "Touch sounds");
        touch_switch(col, "Click on taps", s.tap_sound, [](lv_event_t *event) {
            SoundSettings changed = sound_settings();
            changed.tap_sound = lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED);
            store(changed);
        });
        touch_switch(col, "Tick on sliders", s.slider_sound, [](lv_event_t *event) {
            SoundSettings changed = sound_settings();
            changed.slider_sound = lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED);
            store(changed);
        });
    }
    text_block(col, "Touch sounds play at this volume, none at 0 %. The alarm always rings at full volume.");
    return {"Sound", nullptr};
}

} // namespace hc::ui::settings::pages
