#pragma once

// The pages of Settings. Each builder fills `content` and may return a refresh function that the
// framework calls every second while the page is shown.

#include "lvgl.h"
#include "settings.hpp"

namespace hc::ui::settings::pages {

using Refresh = void (*)();

struct Built {
    const char *title;
    Refresh refresh;
};

Built build(Page page, lv_obj_t *content, const char *text, int index);

} // namespace hc::ui::settings::pages
