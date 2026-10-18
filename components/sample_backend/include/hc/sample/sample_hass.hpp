#pragma once

// The Home Assistant of the sample backend (stub_backend.hpp): sample states for the entities of the dashboard file,
// made up from the domain and the name of each entity (a "..._temperature" sensor reports °C).
// The values of a page arrive 150 ms after it is subscribed, an action changes the sample state
// 400 ms after the tap (--action-ms), so a pending tile is seen for a moment. Every tenth entity is unavailable.
// The history of a sensor panel is made up as well: a daily curve around the sensor's value with
// a gap of an hour, or on/off changes with an unavailable hour; it arrives 300 ms after the request.
// A weather entity reports OpenWeatherMap's values; its forecast is made up as well.
// A camera image is a drawn landscape whose sun moves with the clock, 500 ms after the request.
// The entity search of the edit mode finds made-up entities.
// Nothing arrives while the scenario's Home Assistant is not connected.

#include <memory>
#include <string_view>

#include "hc/core/entity.hpp"
#include "hc/core/layout.hpp"

namespace hc::sample {

// how long an action takes until the sample state changes (--action-ms)
void set_action_ms(uint32_t ms);

// the dashboard of --dashboard; null with `--dashboard none` (the test screen)
void set_dashboard(std::shared_ptr<const hc::core::Layout> layout);

// the Backend functions (hc/ui/backend.hpp)
std::shared_ptr<const hc::core::Layout> dashboard();
// the edit mode's Done: kept in memory as the next revision (the file of --dashboard stays)
bool save_dashboard(const hc::core::Layout &layout, uint32_t revision, char *error, size_t error_size);
void subscribe_page(std::shared_ptr<const hc::core::Layout> layout, hc::core::PageIndex page,
                    hc::core::Orientation o, hc::core::TileIndex panel, uint32_t generation);
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
// The forecast of a weather panel: 8 days or 24 hours from now, the conditions in turn, 300 ms
// after the subscription (once; the real one comes again with every update of the integration).
void subscribe_forecast(std::string_view entity_id, hc::core::EntityIndex entity, hc::core::ForecastType type,
                        uint32_t generation);
// The entity search of the edit mode: the entities of the dashboard and a made-up house of 120
// more, filtered like Home Assistant's template, 300 ms after the request; the details of one
// (lights dim unless their id has "onoff").
void find_entities(std::string_view query, size_t max, uint32_t generation);
void fetch_entity(std::string_view entity_id, uint32_t generation);
// how the forecast answers (step `forecast`): with the entries, never (Loading...), or an error
enum class ForecastMode : uint8_t { Ok, Loading, Error };
void set_forecast_mode(ForecastMode mode);
// The stream's server: down ends the stream that plays ("ended") and refuses new ones
// ("unreachable"); up answers them again (the first frame 1.5 s after the answer).
void set_stream_server(bool up);
// the stream's server answers 401 from now on (StreamProblem::Login, "login needed")
void set_stream_login(bool login);

} // namespace hc::sample
