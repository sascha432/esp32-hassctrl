#include "hc/ui/ui.hpp"

#include "dashboard.hpp"
#include "overlay.hpp"
#include "power.hpp"
#include "rotation.hpp"
#include "settings.hpp"
#include "setup.hpp"
#include "theme.hpp"

namespace hc::ui {

void start(lv_disp_t *disp, const Backend &backend, const TestScreenConfig &test_screen, bool orientation_sensor,
           const Options &options)
{
    theme::use_large_fonts(options.large_fonts);
    // the dashboard when there is one; the test screen until one is imported
    if (backend.dashboard == nullptr || !backend.dashboard()) {
        show_test_screen(disp, test_screen);
    }
    dashboard::init(backend, lv_scr_act(), hide_test_screen, options);
    rotation::init(disp, backend, orientation_sensor);
    settings::init(disp, backend, lv_scr_act());
    power::init(disp, backend);
    setup::init(backend, lv_scr_act());
    overlay::init(backend);
}

void show_setup()
{
    setup::request();
}

void sensor_rotation(uint8_t rotation)
{
    rotation::from_sensor(rotation);
}

} // namespace hc::ui
