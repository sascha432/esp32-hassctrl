#pragma once

// The on-screen keyboard of the Settings forms: LVGL's keyboard with the theme's colors and
// without the "hide keyboard" key. LVGL puts that key in the bottom left corner, next to nothing
// else that leaves a form, where it is hit by mistake instead of shift; the page treated it as
// "cancel" and lost what was typed. The left arrow takes the corner, a miss there only moves the
// cursor.
//
// LV_SYMBOL_OK sends LV_EVENT_READY to the keyboard and then to its text area, Enter to the text
// area only: a form listens on its text areas (settings::pages::set_form).

#include "lvgl.h"

namespace hc::ui::keyboard {

lv_obj_t *create(lv_obj_t *parent);

} // namespace hc::ui::keyboard
