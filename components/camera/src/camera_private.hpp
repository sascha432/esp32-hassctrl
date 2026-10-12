#pragma once

// Shared by the files of the camera component.

#include "hc/camera/camera.hpp"

namespace hc::camera::detail {

// the stream's login changed (web.cpp keeps the Authorization header it expects); called with the
// settings' lock held
void login_changed(const Stream &stream);

// the generation of the newest frame, 0 while the capture is off
uint32_t newest_generation();

} // namespace hc::camera::detail
