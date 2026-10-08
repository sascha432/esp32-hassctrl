#include "hc/uidebug/uidebug.hpp"

#include <cstring>

#include "esp_check.h"
#include "esp_lvgl_port.h"

namespace hc::uidebug {

namespace {

constexpr const char *kTag = "uidebug";

lv_disp_t *g_disp = nullptr;
lv_indev_drv_t g_driver;
// the injected state, in the coordinates of the screen as it is shown
volatile int16_t g_x = 0;
volatile int16_t g_y = 0;
volatile bool g_pressed = false;

// LVGL turns pointer input of a rotated display from the base orientation into the shown one.
// The injected point is in the shown orientation, so it is turned back here first.
void read(lv_indev_drv_t *driver, lv_indev_data_t *data)
{
    const lv_coord_t w = driver->disp->driver->hor_res; // base orientation
    const lv_coord_t h = driver->disp->driver->ver_res;
    const lv_coord_t x = g_x;
    const lv_coord_t y = g_y;
    switch (driver->disp->driver->rotated) {
    case LV_DISP_ROT_90:
        data->point.x = y;
        data->point.y = h - 1 - x;
        break;
    case LV_DISP_ROT_180:
        data->point.x = w - 1 - x;
        data->point.y = h - 1 - y;
        break;
    case LV_DISP_ROT_270:
        data->point.x = w - 1 - y;
        data->point.y = x;
        break;
    default:
        data->point.x = x;
        data->point.y = y;
        break;
    }
    data->state = g_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

struct Rgb {
    uint8_t r, g, b; // 5, 6, 5 bits
};

Rgb rgb_of(const uint8_t *pixel)
{
    lv_color_t color;
    std::memcpy(&color, pixel, sizeof(color));
    return {static_cast<uint8_t>(LV_COLOR_GET_R(color)), static_cast<uint8_t>(LV_COLOR_GET_G(color)),
            static_cast<uint8_t>(LV_COLOR_GET_B(color))};
}

uint16_t blend(Rgb base, Rgb top, uint8_t alpha)
{
    const auto mix = [alpha](uint8_t a, uint8_t b) {
        return static_cast<uint16_t>((b * alpha + a * (255 - alpha)) / 255);
    };
    return static_cast<uint16_t>((mix(base.r, top.r) << 11) | (mix(base.g, top.g) << 5) | mix(base.b, top.b));
}

// collects run-length pairs and hands them out in pieces
class Encoder {
public:
    Encoder(Writer write, void *context) : _write(write), _context(context) {}

    void pixel(uint16_t value)
    {
        if (_count > 0 && value == _value && _count < 255) {
            _count++;
            return;
        }
        flush_run();
        _value = value;
        _count = 1;
    }

    void finish()
    {
        flush_run();
        if (_used > 0) {
            _write(_buffer, _used, _context);
            _used = 0;
        }
    }

private:
    void flush_run()
    {
        if (_count == 0) {
            return;
        }
        if (_used + 3 > sizeof(_buffer)) {
            _write(_buffer, _used, _context);
            _used = 0;
        }
        _buffer[_used++] = _count;
        _buffer[_used++] = static_cast<uint8_t>(_value);
        _buffer[_used++] = static_cast<uint8_t>(_value >> 8);
        _count = 0;
    }

    Writer _write;
    void *_context;
    uint8_t _buffer[765];
    size_t _used = 0;
    uint16_t _value = 0;
    uint8_t _count = 0;
};

} // namespace

esp_err_t init(lv_disp_t *disp)
{
    g_disp = disp;
    lv_indev_drv_init(&g_driver);
    g_driver.type = LV_INDEV_TYPE_POINTER;
    g_driver.read_cb = read;
    g_driver.disp = disp;
    ESP_RETURN_ON_FALSE(lv_indev_drv_register(&g_driver) != nullptr, ESP_FAIL, kTag, "pointer");
    return ESP_OK;
}

esp_err_t screenshot(Writer write, void *context, uint16_t &width, uint16_t &height)
{
    ESP_RETURN_ON_FALSE(g_disp != nullptr, ESP_ERR_INVALID_STATE, kTag, "not initialized");
    lvgl_port_lock(0);
    lv_img_dsc_t *base = lv_snapshot_take(lv_scr_act(), LV_IMG_CF_TRUE_COLOR);
    lv_img_dsc_t *top = lv_snapshot_take(lv_layer_top(), LV_IMG_CF_TRUE_COLOR_ALPHA);
    lvgl_port_unlock();
    if (base == nullptr) {
        if (top != nullptr) {
            lv_snapshot_free(top);
        }
        return ESP_ERR_NO_MEM;
    }

    width = static_cast<uint16_t>(base->header.w);
    height = static_cast<uint16_t>(base->header.h);
    const bool with_top = top != nullptr && top->header.w == width && top->header.h == height;
    Encoder encoder(write, context);
    for (uint32_t i = 0; i < static_cast<uint32_t>(width) * height; i++) {
        const Rgb under = rgb_of(base->data + i * sizeof(lv_color_t));
        if (with_top) {
            const uint8_t *p = top->data + i * LV_IMG_PX_SIZE_ALPHA_BYTE;
            encoder.pixel(blend(under, rgb_of(p), p[LV_IMG_PX_SIZE_ALPHA_BYTE - 1]));
        } else {
            encoder.pixel(blend(under, under, 0));
        }
    }
    encoder.finish();

    lv_snapshot_free(base);
    if (top != nullptr) {
        lv_snapshot_free(top);
    }
    return ESP_OK;
}

void touch(int16_t x, int16_t y, bool pressed)
{
    g_x = x;
    g_y = y;
    g_pressed = pressed;
}

} // namespace hc::uidebug
