#pragma once

// The full screen view of a picture tile (docs/ui/dashboard.md section 6): the camera image the size
// of the display on black, refreshed like the tile; a tap anywhere goes back. Until the first frame
// of that size arrives, the tile's frame is shown enlarged.

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
