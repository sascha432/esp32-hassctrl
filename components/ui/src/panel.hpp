#pragma once

// The panel a tile opens (docs/ui/lvgl/lvgl-panels.md): a full screen view over the page with
// the back tile, the name of the entity and the controls of its kind. One panel is open at a time;
// the dashboard owns which one, subscribes its fields and passes the redraws of its entity on.
//
// The frame is the same for every kind (docs/ui/lvgl/lvgl-panels.md):
//
//   landscape                            portrait
//   +------+---------------------+-+    +--+-----------------+--+
//   | back | name                |a|    |bk| name            |a |
//   +------+---------------------+-+    +--+-----------------+--+
//   | opt- |                       |    |                       |
//   | ions |       control         |    |        control        |
//   |      |                       |    |                       |
//   +------+-----------------------+    +-----------------------+
//                                       |   options (one row)   |
//                                       +-----------------------+
//
// The sensor panel has no options: its frame is the portrait one in both orientations, without the
// options row (the control takes the whole width and height below the header).
//
// `a` is the action of the header (the power button of a light or a climate); without one the name
// takes its place. A panel that hides its action gives the name its place by widening `Frame::name`.
//
// With Options::panel_dialogs the frame is a dialog of 620 x 420 (440 x 600 in portrait) over the
// dimmed page, sliding down from above the screen and back up (not for the camera image); the parts
// are placed in the dialog's coordinates then, and the buttons of the header and the steppers are a
// quarter larger (`Frame::large`).

#include <string>
#include <string_view>

#include "hc/core/entity.hpp"
#include "hc/core/layout.hpp"
#include "hc/memory/large.hpp"
#include "lvgl.h"

namespace hc::ui::panel {

struct Frame {
    lv_obj_t *root = nullptr; // the panel or its dialog; every part is a child of it, placed in its coordinates
    lv_obj_t *name = nullptr; // the name of the entity in `header`, kept up to date by the frame
    lv_area_t back{};
    lv_area_t header{};       // the name, left of `action`
    lv_area_t action{};       // a square at the right end of the header
    lv_area_t options{};      // a column in landscape, a row in portrait
    lv_area_t control{};
    bool portrait = false;
    bool large = false;       // a dialog (the P4): the larger buttons, see step_size()
};

// true when `tile` has a panel (a light with `dimmer`, a climate, a sensor, a lock, a picture: its
// image full screen)
bool has_panel(const core::Tile &tile);

// Builds the panel of `tile` over the page in `parent` (the dashboard's root); a panel that is open
// goes first. Called again after the display turned. `appear`: a dialog slides in (opened by a tap).
void build(lv_obj_t *parent, core::TileIndex tile, bool appear = false);

// removes the panel: a dialog slides out first, the full screen panel goes at once
void close();
// removes the panel at once; nothing happens when none is open
void destroy();

// draws the panel again when it shows `entity`
void update(core::EntityIndex entity);

// a history answer for the history of the sensor or the lock panel
void on_history(core::History history);
// a camera image for the picture panel
void on_image(const core::Image &image);

// ------------------------------------------------------------------------------- shared parts

// the round steppers of a panel and the gap between two of them
constexpr lv_coord_t kStepSize = 64;
constexpr lv_coord_t kStepGap = 8;
// the steppers of a large frame, a quarter larger
constexpr lv_coord_t kStepSizeLarge = kStepSize * 5 / 4;

inline lv_coord_t step_size(const Frame &frame)
{
    return frame.large ? kStepSizeLarge : kStepSize;
}
// holding a stepper steps this often (the touch repeats every 100 ms, too fast to stop on a value)
constexpr uint32_t kStepRepeatMs = 250;
// the color of an action on its way (docs/ui/lvgl/lvgl-dashboard.md section 4; cards show runner.hpp)
constexpr uint32_t kPendingBorder = 0x00ffff;

// a card of the panel in `area`, pressed a shade lighter
lv_obj_t *card(lv_obj_t *parent, const lv_area_t &area);

// The buttons of a panel's controls in `frame.options`: a column in landscape (at most 40 px tall
// each), a row in portrait. Only those with their bit in `shown`; with one or none there is nothing
// to choose, and no button.
void layout_options(const Frame &frame, lv_obj_t *const *buttons, int count, uint32_t shown);
// the button of the control that is shown white, the others as cards
void mark_option(lv_obj_t *button, bool selected);

// The size of a vertical slider (the level and color temperature of a light, the lock) in a
// control of `h` with `room` for its width, and its place from the top.
struct SliderPlace {
    lv_coord_t w;
    lv_coord_t h;
    lv_coord_t y;
};
SliderPlace slider_place(const Frame &frame, lv_coord_t room, lv_coord_t h);
// the track of a vertical slider at x, y: rounded, clipping its fill, dragged without starting a
// gesture of the screen
lv_obj_t *slider_track(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h);

// A round stepper of `size` at x, y with an MDI glyph; `callback` gets LV_EVENT_SHORT_CLICKED and
// LV_EVENT_LONG_PRESSED_REPEAT (throttle the repeat with kStepRepeatMs).
lv_obj_t *step_button(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t size, const char *glyph,
                      lv_event_cb_t callback);

// A list of choices (the effects of a light, the options of a climate) in the area `width` x
// `height` at the top left of `parent`. list_fill() makes its items: one column centered in the
// area when they all fit in it, else two columns that scroll when they need to.
lv_obj_t *list(lv_obj_t *parent, lv_coord_t width, lv_coord_t height);
// One item per value: an optional glyph (`glyph_of` may be nullptr or return nullptr) and the text
// (`text_of(value)`, the value itself when nullptr). `callback` gets LV_EVENT_CLICKED; the item's
// index in the list is the index of its value.
void list_fill(lv_obj_t *list, lv_coord_t width, lv_coord_t height, const memory::Vector<memory::String> &values,
               const char *(*glyph_of)(std::string_view), std::string (*text_of)(std::string_view), lv_event_cb_t callback);
// draws the item of `current` orange, the others as cards
void list_mark(lv_obj_t *list, const memory::Vector<memory::String> &values, std::string_view current);
// scrolls the item of `current` into the middle of the list when it is not in view from the start
void list_reveal(lv_obj_t *list, const memory::Vector<memory::String> &values, std::string_view current,
                 lv_coord_t height);

void set_hidden(lv_obj_t *obj, bool hidden);
// sets the text only when it changed (no redraw otherwise)
void set_text(lv_obj_t *label, const std::string &text);

} // namespace hc::ui::panel
