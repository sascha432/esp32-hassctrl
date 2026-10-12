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
// `format` writes the value label (nullptr: "N %").
using ValueText = void (*)(int32_t value, char *out, size_t size);
lv_obj_t *slider_card(lv_obj_t *parent, const char *name, int32_t min, int32_t max, int32_t value,
                      lv_event_cb_t on_change, ValueText format = nullptr);

// Text fields of a form with the on-screen keyboard (the Wi-Fi password, the camera's login): a
// field of the theme, and a tap on it focuses it and gives it the keyboard. set_form() names the
// form's keyboard and fields (one may be nullptr) and its action: OK and Enter on the keyboard
// call `on_ready` once (LVGL sends OK's LV_EVENT_READY to the keyboard and the field, Enter's to
// the field only, so the fields take it).
lv_obj_t *text_area(lv_obj_t *parent, const char *placeholder, bool password);
void set_form(lv_obj_t *keyboard, lv_obj_t *first, lv_obj_t *second, lv_event_cb_t on_ready);
void focus_area(lv_obj_t *area);

// a paragraph over the full width, wrapped
lv_obj_t *text_block(lv_obj_t *parent, const char *text, uint32_t color = theme::kTextValue);

// the small caption above a group of rows
lv_obj_t *section(lv_obj_t *parent, const char *text);

// the volume slider of Settings, Sound, also on quick settings
void volume_card(lv_obj_t *parent);

// Settings, Sound and Alarm (settings_sound.cpp, settings_alarm.cpp)
Built sound_page(lv_obj_t *content);
Built alarm_page(lv_obj_t *content);

// Settings, Camera, its login and its preview (settings_camera.cpp); camera_available(): the
// backend has a camera with a sensor
bool camera_available();
Built camera_page(lv_obj_t *content);
Built camera_login_page(lv_obj_t *content);
Built camera_preview_page(lv_obj_t *content);

} // namespace hc::ui::settings::pages
