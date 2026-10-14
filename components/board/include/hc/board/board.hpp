#pragma once

// Board support (docs/architecture.md section 8): brings up the panel, the touch controller and
// the backlight with esp_lcd and hands out the handles. No LVGL here, so the recovery app can use
// it too; the main firmware passes the handles to esp_lvgl_port.

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_types.h"

namespace hc::board {

struct Info {
    const char *name;
    // resolution in the base orientation, the one LVGL sees at rotation 0
    uint16_t width;
    uint16_t height;
    bool has_touch;
    // How the panel is mounted: the swap and mirror settings of the panel controller that give the
    // base orientation. Rotations are applied on top of these (docs/architecture.md section 8.1).
    bool swap_xy;
    bool mirror_x;
    bool mirror_y;
    // RGB565 pixels have to be sent high byte first (SPI panels). With LVGL 8 this is
    // CONFIG_LV_COLOR_16_SWAP in the board's sdkconfig.defaults; main.cpp checks that both agree.
    bool swap_bytes;
    // lines of one LVGL draw buffer: the largest band the panel bus takes in one DMA transfer
    // (with full_frames: the bands the frame is sent in)
    uint16_t draw_buffer_lines;
    // The panel only takes whole frames, written from the top, and cannot rotate them: the
    // AXS15231B over QSPI has no row address (a write starts at the first row or continues where
    // the last one ended) and showed an empty screen with MADCTL MV. main then uses
    // hc::frame_display instead of esp_lvgl_port's display: a frame in PSRAM, rotated in software.
    // swap_xy and mirror_* must be false.
    bool full_frames = false;
    // The touch controller is part of the panel controller (the AXS15231B): hc::touch_input reads
    // it in the LVGL task, its own task did not work with it.
    bool touch_in_panel = false;
    // A MIPI-DSI video panel (ST7701S on the ESP32-P4): the DPI controller scans a frame buffer in
    // PSRAM and the panel cannot rotate. main then uses esp_lvgl_port's DSI display, which copies
    // LVGL's bands into the frame buffer, and LVGL rotates in software. swap_xy and mirror_* must
    // be false.
    bool mipi_dsi = false;
    // I2C address of an MPU6050 on the board's I2C bus, 0 without one (docs/architecture.md
    // section 8.1)
    uint8_t imu_address;
    // Backlight PWM frequency and the lowest readable duty of this board type, both found with the
    // backlight tab of the test screen when the board is added (docs/adding-a-board.md). The
    // minimum is the default until a device has a calibration of its own; 0 = none.
    uint32_t backlight_hz;
    uint16_t backlight_min_duty;
    // Light sensor (BH1750, optional): how this board type mounts it, found when the board is
    // added (docs/adding-a-board.md, console `light`). The scale turns what the sensor measures
    // into the light of the room (behind a window or diffuser: above 1). The backlight lux are
    // what the sensor sees of the screen itself at full brightness, subtracted in proportion to
    // the backlight. 1 and 0 when no sensor is built in.
    float light_sensor_scale;
    float light_sensor_backlight_lux;
    // An LDR on this ADC1 GPIO (LDR to 3V3, 10 kOhm to GND) as the light source when no BH1750
    // answers; -1 without one. Calibrated with console `light` (hc::light_sensor::ldr).
    int8_t light_ldr_gpio = -1;
    // The I2S input of a speaker amplifier on the board (the NS4168 of the JC3248W535), -1 without
    // one. The board's Kconfig option selects HC_BOARD_HAS_AUDIO when it has one (hc::audio).
    int8_t audio_bclk_gpio = -1;
    int8_t audio_ws_gpio = -1;
    int8_t audio_dout_gpio = -1;
    // the master clock of a codec, 256 x the sample rate; -1 when nothing needs it
    int8_t audio_mclk_gpio = -1;
    // The I2S data goes through a codec that has to be set up (the ES8311 of the JC4880P443C):
    // without audio_codec() there is no sound.
    bool audio_codec = false;
    // The speaker channel runs all the time instead of stopping a second after a sound: the
    // amplifier's wake-up changed how loud a click was (hc::audio::Config::keep_running).
    bool audio_always_on = false;
    // A vibration motor on this GPIO (through a transistor, high = on), -1 without one. The board's
    // Kconfig option selects HC_BOARD_HAS_VIBRATION when it has one (hc::vibration).
    int8_t vibration_gpio = -1;
    // The pulse of a tap: the shortest one this motor turns into a clear tap, found by the
    // developer with console `vibrate <ms>` when the motor is added (docs/adding-a-board.md).
    // Motors differ a lot: shorter pulses than this one do nothing.
    uint16_t vibration_click_ms = 0;
    // How the MPU6050 sits on this board type, measured by the developer when the board is added
    // (docs/adding-a-board.md, console `orient`): gravity in the module's axes with the panel upright
    // in the base orientation (3 floats), then turned 90 degrees clockwise (3 floats). All zero = the
    // fixed mapping of the old firmware (module mirrored on the back of the panel).
    float imu_axes[6] = {};
    // What the module reads too much in every pose (found at the same time, same console command), g.
    float imu_offset[3] = {};
    // The look of the UI on this board type (hc::ui::Options): dashboard pages slide (every
    // board since 2026-10-14, a swipe fires the slide; false switches without the animation),
    // the panels of a dimmer, a climate and a sensor are dialogs over the page, the fonts are
    // larger (the board's sdkconfig.defaults builds Montserrat 18, 26 and 36 in).
    bool ui_slide_pages = true;
    bool ui_panel_dialogs = false;
    bool ui_large_fonts = false;
    // the gap between the tiles of the dashboard in pixels
    uint8_t ui_tile_gap = 4;
};

// The board an image was built for, at a fixed place in every firmware image: ESP-IDF's custom
// app description, right after esp_app_desc_t (section .rodata_custom_desc). An update is checked
// against the running board with its first bytes, before anything is written (hc::ota).
struct ImageTag {
    uint32_t magic;
    char board[32];
};
constexpr uint32_t kImageTagMagic = 0x54424348; // "HCBT" in the image

// A codec between the I2S output and the amplifier, set up over the board's I2C bus by init().
// The audio output calls it (wired in main): start() whenever its I2S channel starts or changes
// the sample rate, with the master clock already running; stop() before the channel stops.
struct AudioCodec {
    // the codec for 16 bit samples at `sample_rate`, its output and the amplifier on
    esp_err_t (*start)(uint32_t sample_rate);
    // output and amplifier off
    void (*stop)();
};

struct Handles {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    // nullptr when the board has no touch or it did not answer; reports coordinates of the base
    // orientation
    esp_lcd_touch_handle_t touch;
    // the board's shared I2C bus (touch, sensors), nullptr without one
    i2c_master_bus_handle_t i2c;
};

// How a brightness in percent becomes the PWM duty of the backlight
enum class BacklightCurve : uint8_t {
    // duty = percent: equal steps of LED current, which look uneven to the eye
    Linear,
    // the inverse of the CIE lightness: equal steps of perceived brightness (the default)
    Perceptual,
};

const Info &info();

// Initializes the display bus, the panel, the touch controller and the backlight (off; it also
// turns off before every esp_restart()). Call once. A touch controller that does not answer is logged and left out, the display still works.
esp_err_t init(Handles &out);

// The board's audio codec (Info::audio_codec), nullptr without one or when it did not answer.
// After init().
const AudioCodec *audio_codec();

// 0 turns the backlight off. 1..100 are spread from the calibrated minimum to full brightness,
// so no brightness setting makes the screen unreadable.
void set_backlight(uint8_t percent, BacklightCurve curve = BacklightCurve::Perceptual);

// Calibration of a device: the lowest PWM duty at which the screen is still readable, 0 for none.
// Stored with the device's settings, like the orientation sensor calibration.
void set_backlight_minimum(uint16_t duty);
uint16_t backlight_minimum();

// The raw PWM duty, 0..backlight_max_duty(), for finding the minimum.
void set_backlight_duty(uint16_t duty);
uint16_t backlight_max_duty();

// The light the backlight gives now, 0..1 of full (the PWM duty: the luminance is linear in it).
float backlight_level();

// PWM frequency of the backlight (Info::backlight_hz after init), for the backlight test.
esp_err_t set_backlight_frequency(uint32_t hz);

} // namespace hc::board
