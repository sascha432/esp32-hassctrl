#pragma once

// The Backend of the screens (hc/ui/backend.hpp), made of the network, board, platform and web
// server components. Wiring only, like the rest of main.

#include "hc/ui/backend.hpp"

const hc::ui::Backend &ui_backend();
