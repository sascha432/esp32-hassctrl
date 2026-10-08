#pragma once

// Settings (docs/ui/screen.md sections 2, 3 and 5): a screen of its own with the status bar and a
// stack of pages. `<` goes one page back, from quick settings it closes Settings (no swipe up:
// it fired while scrolling). It closes by itself after 5 minutes without a touch, except while a form has unsaved
// input.

#include <cstdint>

#include "hc/ui/backend.hpp"
#include "lvgl.h"

namespace hc::ui::settings {

enum class Page : uint8_t {
    Quick,
    Wifi,
    NetworkDetail,
    AddNetwork,
    Password,
    Hotspot,
    HomeAssistant,
    Display,
    System,
};

void init(lv_disp_t *disp, const Backend &backend, lv_obj_t *dashboard);

// opens Settings at `page` (quick settings below it, so `<` leads there)
void open(Page page = Page::Quick);
void close();
bool is_open();

// Opens Settings with `page` alone on the stack: `<` from it, or the page's own end (the password
// page after Connect), closes Settings back to `return_to` instead of the dashboard. For the
// first setup.
void open_alone(Page page, lv_obj_t *return_to, const char *text = nullptr);

// ------------------------------------------------------------------ used by the pages

// the page a page leads to; `text` is an argument such as the network name of the password page
void push(Page page, const char *text = nullptr, int index = -1);
void pop(int pages = 1);
// builds the shown page again (a list that changed)
void reload();

// a form with unsaved input stops the timeout
void set_form_dirty(bool dirty);

const Backend &backend();

// A modal question with Cancel on the left and the action on the right.
void confirm(const char *title, const char *text, const char *action, bool danger, void (*on_confirm)(void *),
             void *context);

// A small centered window over the page; the caller fills the returned card (a column). A tap
// outside it closes it, and so does `timeout_ms` without popup_touch(). `on_close` follows every
// close (also when Settings closes).
// A square card of a fixed size at the center of the display, with the title at the top; the
// caller places its content with lv_obj_align() (no layout), so nothing moves when the display
// turns.
lv_obj_t *popup(const char *title, uint32_t timeout_ms, void (*on_close)());
// restarts the popup's timeout (a button in it was used)
void popup_touch();
void close_popup();

} // namespace hc::ui::settings
