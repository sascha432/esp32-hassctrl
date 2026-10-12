#pragma once

// The full screen view of a picture tile (docs/ui/lvgl/lvgl-camera.md): the whole camera image on
// black, refreshed like the tile. At 100 % it covers the screen (what sticks out is cut off), the
// zoom goes from 50 to 200 %, a swipe moves the picture. A touch shows the buttons "X" (back), "+"
// and "-" at the top for 5 s. Until the first frame arrives, the tile's frame is shown enlarged.

#include "hc/core/image.hpp"
#include "hc/core/layout.hpp"
#include "lvgl.h"

namespace hc::ui::picture_panel {

// fills `root` (the panel, `width` x `height`) and asks for the image
void build(lv_obj_t *root, core::TileIndex tile, lv_coord_t width, lv_coord_t height);

// a frame for the view; one that answers an older request is dropped
void on_image(const core::Image &image);

// the panel's objects are gone: stops the refresh, lets go of the frame
void forget();

} // namespace hc::ui::picture_panel
