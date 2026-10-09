#pragma once

// The look of an action on its way (docs/ui/dashboard.md section 3): a glowing dot with a short
// fading tail running round the border of a card, in place of a cyan border. It is drawn over the
// card after its children; every frame redraws only the stretch of the border the dot left and
// reached, not the card.

#include "lvgl.h"

namespace hc::ui::runner {

// starts (`on`) or stops the dot on `obj`; nothing happens when it already is so
void set(lv_obj_t *obj, bool on);

} // namespace hc::ui::runner
