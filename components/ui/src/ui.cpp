#include "hc/ui/ui.hpp"

#include "dashboard.hpp"
#include "overlay.hpp"
#include "power.hpp"
#include "rotation.hpp"
#include "settings.hpp"
#include "setup.hpp"
#include "splash.hpp"
#include "theme.hpp"

namespace hc::ui {

void start(lv_disp_t *disp, const Backend &backend, bool orientation_sensor, const Options &options,
           void (*before_dashboard)())
{
    theme::use_large_fonts(options.large_fonts);
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, theme::color(theme::kBackground), 0);
    // the start screen lies over the dashboard until Home Assistant has sent its values
    splash::init(backend, screen);
    dashboard::init(backend, screen, before_dashboard, options);
    rotation::init(disp, backend, orientation_sensor);
    settings::init(disp, backend, screen);
    power::init(disp, backend);
    setup::init(backend, screen);
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
