#pragma once

#include <string>

#include "hc/core/layout.hpp"

namespace sim {

// Reads a hass.yaml like the web UI's import does: the file and every .yaml file below its folder
// (the includes), through hc::config::import_yaml, so a mistake in it is reported with file and
// line the same way. On success `layout` holds the dashboard; on failure `error` says why.
bool load_dashboard(const std::string &path, hc::core::Layout &layout, std::string &error);

} // namespace sim
