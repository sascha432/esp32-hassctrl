#pragma once

// The pages of Settings. Each builder fills `content` and may return a refresh function that the
// framework calls every second while the page is shown.

#include "lvgl.h"
#include "settings.hpp"
#include "theme.hpp"

namespace hc::ui::settings::pages {

using Refresh = void (*)();

struct Built {
    const char *title;
    Refresh refresh;
};

Built build(Page page, lv_obj_t *content, const char *text, int index);

// ------------------------------------------------------------- shared by the page files

// A card: the name and the value in the first line, the slider over the full width below, so the
// slider stays usable on narrow screens. `on_change` gets the slider; the value label is updated
// here.
lv_obj_t *slider_card(lv_obj_t *parent, const char *name, int32_t min, int32_t max, int32_t value,
                      lv_event_cb_t on_change);

// a paragraph over the full width, wrapped
lv_obj_t *text_block(lv_obj_t *parent, const char *text, uint32_t color = theme::kTextValue);

// the small caption above a group of rows
lv_obj_t *section(lv_obj_t *parent, const char *text);

// the volume slider of Settings, Sound, also on quick settings
void volume_card(lv_obj_t *parent);

// Settings, Sound and Alarm (settings_sound.cpp, settings_alarm.cpp)
Built sound_page(lv_obj_t *content);
Built alarm_page(lv_obj_t *content);

} // namespace hc::ui::settings::pages
