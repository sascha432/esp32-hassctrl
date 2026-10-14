#pragma once

// The tiles of a dashboard page (docs/ui/lvgl/lvgl-dashboard.md): one view per tile, drawn from the state of
// its entity and the action that is pending for it. The gestures of a tile turn into intents
// through the dashboard (dashboard.hpp).

#include <cstdint>
#include <string>
#include <string_view>

#include "hc/core/entity.hpp"
#include "hc/core/layout.hpp"
#include "hc/core/pending.hpp"
#include "hc/core/image.hpp"
#include "hc/memory/large.hpp"
#include "lvgl.h"
#include "lvgl_compat.hpp"

namespace hc::ui::tiles {

struct View {
    core::TileIndex tile = core::kNone; // kNone: the back tile of an area page
    core::EntityIndex entity = core::kNone;
    core::TileKind kind = core::TileKind::Spacer;
    bool large = false;    // the cell is large enough for the large fonts
    bool tall = false;     // spans two rows (dimmer and climate 1x2)
    bool wide = false;     // a weather tile of two columns: the icon beside the text
    lv_obj_t *card = nullptr;
    lv_obj_t *icon = nullptr;
    lv_obj_t *value = nullptr;
    lv_coord_t value_width = 0;  // the width the value may take
    lv_obj_t *name = nullptr;
    lv_obj_t *fill = nullptr;    // dimmer: the level
    lv_coord_t inner_height = 0; // dimmer: the height of a full fill
    lv_obj_t *plus = nullptr;    // climate: the step bars
    lv_obj_t *minus = nullptr;
    lv_obj_t *action = nullptr;  // climate: "Heating"
    lv_obj_t *current = nullptr; // climate: the room temperature
    memory::String shown_name;

    // picture: the camera image over the whole card, the name on a band at its bottom
    lv_obj_t *image = nullptr;
    lv_obj_t *caption = nullptr;
    compat::ImageDsc image_dsc{};
    core::Pixels pixels;
    uint16_t image_width = 0;  // the frame that is asked for
    uint16_t image_height = 0;
    // the picture the frame was cut from (core::Image::source_width), for the full screen view
    uint16_t source_width = 0;
    uint16_t source_height = 0;
    uint32_t image_generation = 0;
    uint32_t image_asked = 0; // the tick of the request on its way, 0: none
    lv_timer_t *image_timer = nullptr;

    // gestures
    lv_coord_t press_y = 0;
    bool dragging = false;
    int drag_level = 0;          // dimmer, percent while dragging
    uint32_t last_tap = 0;
    lv_timer_t *tap_timer = nullptr;
    float step_target = 0;       // climate, while the steps are collected
    lv_timer_t *send_timer = nullptr;
};

// the state of a binary sensor in the words of its device class, as the HA frontend has them
const char *binary_words(std::string_view device_class, bool on);

// The value of a sensor tile: a binary sensor in words, a number with the tile's decimals and the
// unit, a text with a capital (its first line).
std::string sensor_text(const core::Tile &tile, std::string_view entity_id, const core::EntityState &state);

// "heat_cool" -> "Heat cool": the states of HA as words
std::string words(std::string_view state);

// a setpoint with the fraction of its step: "21.5", "22"; "--" when unknown
std::string temperature(float value);

// the setpoint step of a climate: the entity's target_temp_step, else the tile's step, else 0.5
float climate_step(const core::Tile &tile, const core::EntityState &state);

// The level of a light in percent as the screens show it, the pending action's before the entity's;
// `on` is set as well. 0 while off, 100 for a light without a level, -1 while it is switched on
// and the level it comes on at is not known yet.
int shown_level(const core::EntityState &state, const core::Expectation *pending, bool &on);

// Creates the view of `view.tile` (or the back tile) in the area x, y, w, h of `parent`.
void create(View &view, lv_obj_t *parent, const core::Layout &layout, lv_area_t area);

// asks for the camera image of a picture view now (after a panel covered the page)
void request_image(View &view);

// A camera image for a picture view; one that answers an older request is dropped.
void show_image(View &view, const core::Image &image);

// Draws the view from its entity's state; `pending` is the action on its way, or nullptr.
void update(View &view, const core::Layout &layout, const core::EntityState &state, const core::Expectation *pending);

} // namespace hc::ui::tiles
