#pragma once

// The icons a tile can name (docs/architecture.md section 5, "Tile format"): Material Design
// Icons by their MDI names, one name per glyph. The fonts of the firmware hold these glyphs; the
// list grows with the HA defaults of the domains and device classes once their flash cost is
// measured.

#include <span>
#include <string_view>

namespace hc::core {

// every name a tile may use in `icon`, besides "none"
std::span<const std::string_view> icon_names();

bool known_icon(std::string_view name);

} // namespace hc::core
