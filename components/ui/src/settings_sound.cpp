// Settings, Sound (docs/ui/lvgl/lvgl-settings.md section 3, Sound): only with an audio output or a vibration
// motor (sound::page_available()). The volume is also on quick settings (volume_card()). The
// Microphone section only with one (Backend::mic_settings), its meter only while the page shows.

#include <algorithm>
#include <cstdio>

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

// ------------------------------------------------------------------------------- microphone

// The meter from -60 dBFS to full scale; the mark at the ALC's lower level (es8311.cpp,
// kAlcMinLevel), where speech should be; above -6 dBFS it is close to clipping.
constexpr int32_t kMeterFloorDbfs = -60;
constexpr int32_t kMeterTargetDbfs = -18;
constexpr float kMeterHotDbfs = -6.0f;
constexpr uint32_t kMeterMs = 100;
// a peak falls back this much per update, so a short sound stays readable
constexpr float kMeterFallDb = 3.0f;

// "Volume is 0" stays on the record button this long after a tap that could not record
constexpr uint32_t kRecordRefusedMs = 2000;

struct Mic {
    lv_obj_t *gain_card = nullptr;
    lv_obj_t *gain_slider = nullptr;
    lv_obj_t *bar = nullptr;
    lv_obj_t *value = nullptr;
    lv_obj_t *record = nullptr;
    lv_timer_t *timer = nullptr;
    float shown = kMeterFloorDbfs;
    // when a tap on Record was refused, 0: not
    uint32_t refused_at = 0;
};

Mic g_mic;

MicSettings mic_settings()
{
    MicSettings m{};
    backend().mic_settings(m);
    return m;
}

// the fixed gain counts only without Auto gain: the slider greyed out and without touches then
void show_gain(bool auto_gain)
{
    if (auto_gain) {
        lv_obj_add_state(g_mic.gain_slider, LV_STATE_DISABLED);
        lv_obj_set_style_opa(g_mic.gain_card, LV_OPA_40, 0);
    } else {
        lv_obj_clear_state(g_mic.gain_slider, LV_STATE_DISABLED);
        lv_obj_set_style_opa(g_mic.gain_card, LV_OPA_COVER, 0);
    }
}

// the record button's text and whether it takes a tap, from the recording's state
void record_update()
{
    if (g_mic.record == nullptr) {
        return;
    }
    uint32_t remaining_ms = 0;
    const MicRecording state = backend().mic_recording(remaining_ms);
    char text[24];
    if (state == MicRecording::Recording) {
        std::snprintf(text, sizeof(text), "Stop (%u s)", static_cast<unsigned>((remaining_ms + 999) / 1000));
    } else if (state == MicRecording::Playing) {
        std::snprintf(text, sizeof(text), "Playing...");
    } else if (g_mic.refused_at != 0 && lv_tick_elaps(g_mic.refused_at) < kRecordRefusedMs) {
        std::snprintf(text, sizeof(text), "Volume is 0");
    } else {
        g_mic.refused_at = 0;
        std::snprintf(text, sizeof(text), "Record");
    }
    lv_label_set_text(lv_obj_get_child(g_mic.record, 0), text);
    if (state == MicRecording::Playing) {
        lv_obj_add_state(g_mic.record, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(g_mic.record, LV_STATE_DISABLED);
    }
}

void on_record(lv_event_t *)
{
    uint32_t remaining_ms = 0;
    const MicRecording state = backend().mic_recording(remaining_ms);
    if (state == MicRecording::Recording) {
        backend().mic_record(false);
    } else if (state == MicRecording::Idle && !backend().mic_record(true)) {
        g_mic.refused_at = std::max<uint32_t>(1, lv_tick_get());
    }
    record_update();
}

void meter_update(lv_timer_t *)
{
    float dbfs = kMeterFloorDbfs;
    const bool heard = backend().mic_level(dbfs);
    g_mic.shown = std::max({heard ? dbfs : static_cast<float>(kMeterFloorDbfs), g_mic.shown - kMeterFallDb,
                            static_cast<float>(kMeterFloorDbfs)});
    lv_bar_set_value(g_mic.bar, static_cast<int32_t>(g_mic.shown), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_mic.bar, t::color(g_mic.shown > kMeterHotDbfs ? t::kAlert : t::kAccent),
                              LV_PART_INDICATOR);
    char text[16];
    if (g_mic.shown <= kMeterFloorDbfs) {
        std::snprintf(text, sizeof(text), "-");
    } else {
        std::snprintf(text, sizeof(text), "%d dBFS", static_cast<int>(g_mic.shown));
    }
    lv_label_set_text(g_mic.value, text);
    record_update();
}

// the level of the last moments, with a mark where speech should be
void meter_card(lv_obj_t *parent)
{
    lv_obj_t *card = t::plain(parent);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, t::color(t::kCard), 0);
    lv_obj_set_style_radius(card, t::radius(), 0);
    lv_obj_set_style_pad_hor(card, t::dim(12), 0);
    lv_obj_set_style_pad_ver(card, t::dim(10), 0);
    lv_obj_set_style_pad_row(card, t::dim(8), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *head = t::plain(card);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    t::label(head, "Level", t::font_medium(), t::kText);
    g_mic.value = t::label(head, "-", t::font_normal(), t::kTextValue);

    g_mic.bar = lv_bar_create(card);
    lv_obj_set_size(g_mic.bar, lv_pct(100), t::dim(10));
    lv_bar_set_range(g_mic.bar, kMeterFloorDbfs, 0);
    lv_bar_set_value(g_mic.bar, kMeterFloorDbfs, LV_ANIM_OFF);
    lv_obj_set_style_radius(g_mic.bar, t::dim(5), 0);
    lv_obj_set_style_radius(g_mic.bar, t::dim(5), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g_mic.bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g_mic.bar, t::color(t::kBorder), 0);
    lv_obj_set_style_bg_opa(g_mic.bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_mic.bar, t::color(t::kAccent), LV_PART_INDICATOR);
    lv_obj_t *mark = t::plain(g_mic.bar);
    lv_obj_set_size(mark, t::dim(2), lv_pct(100));
    lv_obj_set_x(mark, lv_pct((kMeterTargetDbfs - kMeterFloorDbfs) * 100 / -kMeterFloorDbfs));
    lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(mark, t::color(t::kText), 0);
}

void mic_section(lv_obj_t *col)
{
    MicSettings m{};
    if (backend().mic_settings == nullptr || !backend().mic_settings(m)) {
        return;
    }
    section(col, "Microphone");
    meter_card(col);
    if (backend().mic_record != nullptr) {
        g_mic.record = t::button(button_row(col), "Record", on_record);
    }
    g_mic.gain_slider = slider_card(
        col, "Gain", 0, kMicMaxGainDb, m.gain_db,
        [](lv_event_t *event) {
            MicSettings changed = mic_settings();
            changed.gain_db = static_cast<uint8_t>(lv_slider_get_value(lv_event_get_target(event)));
            backend().set_mic_settings(changed);
        },
        [](int32_t value, char *out, size_t size) { std::snprintf(out, size, "%d dB", static_cast<int>(value)); });
    // slider, track, card
    g_mic.gain_card = lv_obj_get_parent(lv_obj_get_parent(g_mic.gain_slider));
    show_gain(m.auto_gain);
    touch_switch(col, "Auto gain", m.auto_gain, [](lv_event_t *event) {
        MicSettings changed = mic_settings();
        changed.auto_gain = lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED);
        backend().set_mic_settings(changed);
        show_gain(changed.auto_gain);
    });
    text_block(col, "Auto gain keeps a voice at the mark whatever its distance. Without it the gain stays "
                    "where it is set, and the level follows the room. Record takes up to 5 s and plays them "
                    "right after, at this volume.");

    // the samples are read only while the meter shows
    g_mic.shown = kMeterFloorDbfs;
    backend().mic_listen(true);
    g_mic.timer = lv_timer_create(meter_update, kMeterMs, nullptr);
    lv_obj_add_event_cb(
        col,
        [](lv_event_t *) {
            lv_timer_del(g_mic.timer);
            backend().mic_listen(false);
            // a recording ends with the page; one that plays plays on
            if (backend().mic_record_cancel != nullptr) {
                backend().mic_record_cancel();
            }
            g_mic = {};
        },
        LV_EVENT_DELETE, nullptr);
}

// the motor's switch, only with one
void vibration_section(lv_obj_t *col)
{
    if (!sound::vibration_available()) {
        return;
    }
    bool on = false;
    backend().vibration(on);
    section(col, "Vibration");
    // the tap that turns it on pulses already: the feedback comes after the switch changed
    touch_switch(col, "Vibrate", on, [](lv_event_t *event) {
        backend().set_vibration(lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED));
        sound::settings_changed();
    });
    text_block(col, "A short pulse with every tap. Each alarm has a Vibrate switch of its own.");
}

} // namespace

void volume_card(lv_obj_t *parent)
{
    lv_obj_t *slider = slider_card(parent, "Volume", 0, 100, sound_settings().volume, [](lv_event_t *event) {
        SoundSettings changed = sound_settings();
        changed.volume = static_cast<uint8_t>(lv_slider_get_value(lv_event_get_target(event)));
        store(changed);
    });
    // the new volume to hear: the click when the slider is let go, unless both touch sounds are off
    lv_obj_add_event_cb(
        slider,
        [](lv_event_t *) {
            const SoundSettings s = sound_settings();
            if (backend().play_touch_sound != nullptr && s.volume > 0 && (s.tap_sound || s.slider_sound)) {
                backend().play_touch_sound();
            }
        },
        LV_EVENT_RELEASED, nullptr);
}

Built sound_page(lv_obj_t *content)
{
    lv_obj_t *col = t::column(content);
    if (!sound::available()) {
        vibration_section(col);
        return {"Sound", nullptr};
    }
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
    mic_section(col);
    vibration_section(col);
    return {"Sound", nullptr};
}

} // namespace hc::ui::settings::pages
