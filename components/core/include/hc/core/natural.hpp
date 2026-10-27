#pragma once

// The order of names in the lists of files (the file manager, the media browser, the player's
// folders): without case, a run of digits by its value ("2 x" before "10 x"). Plain C++, host tested.

#include <string_view>

namespace hc::core {

// `a` comes before `b`; numbers of equal value with more leading zeros come after
bool natural_less(std::string_view a, std::string_view b);

} // namespace hc::core
