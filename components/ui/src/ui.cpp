#include "hc/ui/ui.hpp"

#include <iterator>

#include "alarm.hpp"
#include "dashboard.hpp"
#include "overlay.hpp"
#include "power.hpp"
#include "rotation.hpp"
#include "settings.hpp"
#include "setup.hpp"
#include "sound.hpp"
#include "splash.hpp"
#include "theme.hpp"

namespace hc::ui {

namespace {

// LVGL finds the object under a touch by the objects' places, which its layout sets only before
// the next frame. Tested against older places, a second tap opened the tile under a panel again
// (your reports, 2026-11-08): read right after the tap that built the panel, before its first
// frame (taps queued while it was built); or held through the end of the panel's slide, when the
// cover under the panel goes and LVGL looks again under the finger while the panel stood where
// the last frame (9 per second on the JC3248W535) had it. Every read of a pointer with the finger
// down lays the screen out first (nothing to do when nothing changed): the touch screen's and the
// Windows version's, those that exist when the UI starts (not the console's).
struct Reader {
    lv_indev_drv_t *driver;
    void (*read)(lv_indev_drv_t *, lv_indev_data_t *);
};
Reader g_readers[2];

void read_laid_out(lv_indev_drv_t *driver, lv_indev_data_t *data)
{
    for (const Reader &reader : g_readers) {
        if (reader.driver == driver) {
            reader.read(driver, data);
            break;
        }
    }
    if (data->state == LV_INDEV_STATE_PRESSED) {
        lv_disp_t *disp = driver->disp != nullptr ? driver->disp : lv_disp_get_default();
        lv_obj_update_layout(lv_disp_get_scr_act(disp));
        lv_obj_update_layout(lv_disp_get_layer_top(disp));
        lv_obj_update_layout(lv_disp_get_layer_sys(disp));
    }
}

void lay_out_before_touches()
{
    size_t count = 0;
    for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev != nullptr && count < std::size(g_readers);
         indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER && indev->driver->read_cb != read_laid_out) {
            g_readers[count++] = {indev->driver, indev->driver->read_cb};
            indev->driver->read_cb = read_laid_out;
        }
    }
}

} // namespace

void start(lv_disp_t *disp, const Backend &backend, bool orientation_sensor, const Options &options,
           void (*before_dashboard)())
{
    lay_out_before_touches();
    theme::use_large_fonts(options.large_fonts);
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, theme::color(theme::kBackground), 0);
    // the start screen lies over the dashboard until Home Assistant has sent its values
    splash::init(backend, screen);
    dashboard::init(backend, screen, before_dashboard, options);
    rotation::init(disp, backend, orientation_sensor);
    settings::init(disp, backend, screen);
    power::init(disp, backend);
    sound::init(backend);
    alarm::init(backend);
    setup::init(backend, screen);
    overlay::init(backend);
}

void show_setup()
{
    setup::request();
}

void ring_alarm()
{
    alarm::request_ring();
}

void settings_imported()
{
    settings::close();
    rotation::settings_changed();
    power::settings_changed();
    sound::settings_changed();
}

void sensor_rotation(uint8_t rotation)
{
    rotation::from_sensor(rotation);
}

} // namespace hc::ui
