#include "rotation.hpp"

namespace hc::ui::rotation {

namespace {

struct Context {
    lv_disp_t *disp = nullptr;
    const Backend *backend = nullptr;
    bool sensor = false;
    // the sensor's last rotation, -1 before the first one
    int sensor_rotation = -1;
};

Context g;

DisplaySettings settings()
{
    DisplaySettings s{};
    g.backend->display_settings(s);
    return s;
}

void show(uint8_t rotation)
{
    if (lv_disp_get_rotation(g.disp) != static_cast<lv_disp_rot_t>(rotation)) {
        lv_disp_set_rotation(g.disp, static_cast<lv_disp_rot_t>(rotation & 3));
    }
}

} // namespace

void init(lv_disp_t *disp, const Backend &backend, bool sensor)
{
    // keeps a sensor rotation that arrived before (the sensor starts first)
    const int early = g.sensor_rotation;
    g = {};
    g.disp = disp;
    g.backend = &backend;
    g.sensor = sensor;
    g.sensor_rotation = early;
    const DisplaySettings s = settings();
    // until the sensor reports (or when it is gone), the last rotation: the stored one
    show(sensor && !s.rotation_locked && early >= 0 ? static_cast<uint8_t>(early) : s.rotation);
}

void from_sensor(uint8_t rotation)
{
    g.sensor_rotation = rotation;
    if (g.backend == nullptr) {
        return; // before init(), which takes it over
    }
    DisplaySettings s = settings();
    if (s.rotation_locked) {
        return;
    }
    show(rotation);
    // the next start begins in this rotation until the sensor reports
    if (s.rotation != rotation) {
        s.rotation = rotation;
        g.backend->set_display_settings(s);
    }
}

void settings_changed()
{
    const DisplaySettings s = settings();
    show(g.sensor && !s.rotation_locked && g.sensor_rotation >= 0 ? static_cast<uint8_t>(g.sensor_rotation)
                                                                    : s.rotation);
}

void rotate_clockwise()
{
    // LVGL's steps turn the content counterclockwise (the panel turned clockwise), so one step
    // clockwise is three of them
    DisplaySettings s = settings();
    s.rotation = static_cast<uint8_t>((lv_disp_get_rotation(g.disp) + 3) & 3);
    if (g.sensor) {
        s.rotation_locked = true;
    }
    g.backend->set_display_settings(s);
    show(s.rotation);
}

void set_locked(bool locked)
{
    DisplaySettings s = settings();
    s.rotation_locked = locked;
    s.rotation = static_cast<uint8_t>(lv_disp_get_rotation(g.disp));
    g.backend->set_display_settings(s);
    if (!locked && g.sensor_rotation >= 0) {
        show(static_cast<uint8_t>(g.sensor_rotation));
    }
}

bool has_sensor()
{
    return g.sensor;
}

bool locked()
{
    return settings().rotation_locked;
}

uint16_t degrees()
{
    // LVGL's rotation turns the content counterclockwise
    return static_cast<uint16_t>(((4 - lv_disp_get_rotation(g.disp)) & 3) * 90);
}

} // namespace hc::ui::rotation
