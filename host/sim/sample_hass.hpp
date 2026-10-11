#pragma once

// The Home Assistant of the Windows version: sample states for the entities of the dashboard file,
// made up from the domain and the name of each entity (a "..._temperature" sensor reports °C).
// The values of a page arrive 150 ms after it is subscribed, an action changes the sample state
// 400 ms after the tap (--action-ms), so a pending tile is seen for a moment. Every tenth entity is unavailable.
// The history of a sensor panel is made up as well: a daily curve around the sensor's value with
// a gap of an hour, or on/off changes with an unavailable hour; it arrives 300 ms after the request.
// A camera image is a drawn landscape whose sun moves with the clock, 500 ms after the request.
// Nothing arrives while the scenario's Home Assistant is not connected.

#include <memory>
#include <string_view>

#include "hc/core/entity.hpp"
#include "hc/core/layout.hpp"

namespace sim {

// how long an action takes until the sample state changes (--action-ms)
void set_action_ms(uint32_t ms);

// the dashboard of --dashboard; null with `--dashboard none` (the test screen)
void set_dashboard(std::shared_ptr<const hc::core::Layout> layout);

// the Backend functions (hc/ui/backend.hpp)
std::shared_ptr<const hc::core::Layout> dashboard();
// the edit mode's Done: kept in memory as the next revision (the file of --dashboard stays)
bool save_dashboard(const hc::core::Layout &layout, uint32_t revision, char *error, size_t error_size);
void subscribe_page(std::shared_ptr<const hc::core::Layout> layout, hc::core::PageIndex page,
                    hc::core::TileIndex panel, uint32_t generation);
void call_service(const hc::core::Intent &intent, std::string_view entity_id);
bool take_event(hc::core::HassEvent &out);
void fetch_history(std::string_view entity_id, hc::core::EntityIndex entity, hc::core::HistoryKind kind, uint32_t hours,
                   uint32_t generation);
void fetch_image(std::string_view entity_id, std::string_view stream, hc::core::EntityIndex entity, uint16_t width,
                 uint16_t height, bool whole, uint32_t generation);
// a stream plays as the same landscape, 5 frames a second, the sun moving with every frame
void play_stream(std::string_view entity_id, std::string_view stream, hc::core::EntityIndex entity, uint16_t width,
                 uint16_t height, uint32_t generation);
void stop_stream();

} // namespace sim
