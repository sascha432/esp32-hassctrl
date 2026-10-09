// Wireless-Tag WT32-SC01: ESP32-WROVER-B, 4 MB flash, 4 MB PSRAM, 3.5" 320x480 ST7796S over SPI
// (write only), FT6336U touch and an MPU6050 on the shared I2C bus.
// Pins: docs/devices/WT32-SC01/WT32-SC01.md. Panel and touch settings taken from the working
// LovyanGFX configuration of kfc_fw (src/plugins/lvgl/devices/wt32_sc01/lgfx_wt32_sc01.h).

#include "hc/board/board.hpp"

#include <string_view>

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"

#include "backlight.hpp"

// the board tag of every image built for this board (hc::board::ImageTag); CMakeLists.txt keeps it
extern "C" constexpr hc::board::ImageTag hc_image_tag __attribute__((section(".rodata_custom_desc"), used)) = {
    hc::board::kImageTagMagic, "WT32-SC01"};

namespace hc::board {

namespace {

constexpr const char *kTag = "board";

// The controller memory is portrait 320x480. The base orientation is landscape 480x320, the
// orientation of the old firmware (LovyanGFX rotation 1): row/column exchange, no mirroring.
constexpr Info kInfo{
    .name = "WT32-SC01",
    .width = 480,
    .height = 320,
    .has_touch = true,
    .swap_xy = true,
    .mirror_x = false,
    .mirror_y = false,
    .swap_bytes = true,
    .draw_buffer_lines = 40,
    .imu_address = 0x68,
    // 1, 5 and 25 kHz looked the same on the WT32-SC01 (2026-10-06)
    .backlight_hz = 25000,
    // lowest readable duty: readable in daylight, easy in the dark (2026-10-06)
    .backlight_min_duty = 10,
    // no sensor built in: a module on a cable sees the room as it is
    .light_sensor_scale = 1.0f,
    .light_sensor_backlight_lux = 0.0f,
    // LDR + 10k to GND on the header, replaced unit of 2026-10-07
    .light_ldr_gpio = 34,
    // MPU6050 of the replaced unit, measured with `orient capture` on 2026-10-07 (gain 0.99, 0 and 90
    // 89.9 degrees apart, tilt 2.5 degrees): it reads (-0.12, -0.53, +0.21) g too much in every pose.
    // The axes are those of the measurement turned by 180 degrees: it was taken with the panel held
    // the other way up than the screen reads at rotation 0.
    .imu_axes = {0.0106f, 0.9999f, 0.0131f, 0.9991f, -0.0111f, 0.0418f},
    .imu_offset = {-0.1248f, -0.5314f, 0.2124f},
};
// the image tag at the top of this file names the board the same way
static_assert(std::string_view(kInfo.name) == std::string_view(hc_image_tag.board));

constexpr uint16_t kNativeWidth = 320;
constexpr uint16_t kNativeHeight = 480;

// the HSPI pins of the IO MUX, which is what allows 80 MHz
constexpr spi_host_device_t kSpiHost = SPI2_HOST;
constexpr int kPixelClockHz = 80 * 1000 * 1000;
constexpr int kMaxTransferBytes = kNativeHeight * kInfo.draw_buffer_lines * 2;

constexpr gpio_num_t kPinSclk = GPIO_NUM_14;
constexpr gpio_num_t kPinMosi = GPIO_NUM_13;
constexpr gpio_num_t kPinCs = GPIO_NUM_15;
constexpr gpio_num_t kPinDc = GPIO_NUM_21;
constexpr gpio_num_t kPinReset = GPIO_NUM_22;
constexpr gpio_num_t kPinBacklight = GPIO_NUM_23;

// shared I2C bus: FT6336U touch (0x38) and the MPU6050 (0x68), 10k pull-ups on the board
constexpr i2c_port_num_t kI2cPort = I2C_NUM_0;
constexpr gpio_num_t kPinSda = GPIO_NUM_18;
constexpr gpio_num_t kPinScl = GPIO_NUM_19;
constexpr uint32_t kI2cHz = 400000;

esp_err_t init_panel(Handles &out)
{
    spi_bus_config_t bus{};
    bus.sclk_io_num = kPinSclk;
    bus.mosi_io_num = kPinMosi;
    bus.miso_io_num = -1; // write only panel
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = kMaxTransferBytes;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(kSpiHost, &bus, SPI_DMA_CH_AUTO), kTag, "SPI bus");

    esp_lcd_panel_io_spi_config_t io_config{};
    io_config.cs_gpio_num = kPinCs;
    io_config.dc_gpio_num = kPinDc;
    io_config.spi_mode = 0;
    io_config.pclk_hz = kPixelClockHz;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kSpiHost), &io_config, &out.io), kTag,
        "panel IO");

    esp_lcd_panel_dev_config_t panel_config{};
    panel_config.reset_gpio_num = kPinReset;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_config.bits_per_pixel = 16;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7796(out.io, &panel_config, &out.panel), kTag, "ST7796");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(out.panel), kTag, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(out.panel), kTag, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(out.panel, kInfo.swap_xy), kTag, "swap_xy");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(out.panel, kInfo.mirror_x, kInfo.mirror_y), kTag, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(out.panel, false), kTag, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(out.panel, true), kTag, "display on");
    return ESP_OK;
}

esp_err_t init_i2c(Handles &out)
{
    i2c_master_bus_config_t bus{};
    bus.i2c_port = kI2cPort;
    bus.sda_io_num = kPinSda;
    bus.scl_io_num = kPinScl;
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = true;
    return i2c_new_master_bus(&bus, &out.i2c);
}

esp_err_t init_touch(Handles &out)
{
    esp_lcd_panel_io_i2c_config_t io_config{};
    io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS;
    io_config.control_phase_bytes = 1;
    io_config.dc_bit_offset = 0;
    io_config.lcd_cmd_bits = 8;
    io_config.flags.disable_control_phase = 1;
    io_config.scl_speed_hz = kI2cHz;
    // The touch is read from the LVGL task. Without a timeout esp_lcd waits forever, and a module
    // on the shared bus that loses its supply holds SDA/SCL low: the screen froze until it was
    // plugged in again. A read takes about 0.2 ms; 50 ms leaves room for a busy bus.
    io_config.transaction_timeout_ms = 50;
    esp_lcd_panel_io_handle_t io = nullptr;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(out.i2c, &io_config, &io), kTag, "touch IO");

    // The controller reports portrait coordinates (0..319, 0..479). The base orientation is the
    // row/column exchange of the panel: x = y_raw, y = 319 - x_raw. esp_lcd_touch mirrors first
    // and swaps afterwards, so: mirror x against 319, then swap.
    // The INT line cannot be used on this board (GPIO39 floats, no pull-up), the driver polls.
    esp_lcd_touch_config_t config{};
    config.x_max = kNativeWidth - 1;
    config.y_max = kNativeHeight - 1;
    config.rst_gpio_num = GPIO_NUM_NC;
    config.int_gpio_num = GPIO_NUM_NC;
    config.flags.swap_xy = 1;
    config.flags.mirror_x = 1;
    config.flags.mirror_y = 0;
    return esp_lcd_touch_new_i2c_ft5x06(io, &config, &out.touch);
}

} // namespace

const Info &info()
{
    return kInfo;
}

const AudioCodec *audio_codec()
{
    return nullptr; // no audio
}

esp_err_t init(Handles &out)
{
    out = {};
    ESP_RETURN_ON_ERROR(detail::backlight_init(kPinBacklight, kInfo.backlight_hz, kInfo.backlight_min_duty), kTag, "backlight");
    ESP_RETURN_ON_ERROR(init_panel(out), kTag, "panel");
    detail::set_restart_panel(out.panel);
    ESP_LOGI(kTag, "%s: ST7796S %ux%u, SPI %d MHz", kInfo.name, kNativeWidth, kNativeHeight, kPixelClockHz / 1000000);

    if (init_i2c(out) != ESP_OK) {
        ESP_LOGW(kTag, "I2C bus failed, no touch and no sensors");
        out.i2c = nullptr;
        return ESP_OK;
    }
    if (init_touch(out) == ESP_OK) {
        ESP_LOGI(kTag, "FT6336U touch at 0x%02x, polled", ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS);
    } else {
        ESP_LOGW(kTag, "touch controller did not answer");
        out.touch = nullptr;
    }
    return ESP_OK;
}

void set_backlight(uint8_t percent, BacklightCurve curve)
{
    detail::backlight_set(percent, curve);
}

void set_backlight_minimum(uint16_t duty)
{
    detail::backlight_set_minimum(duty);
}

uint16_t backlight_minimum()
{
    return detail::backlight_minimum();
}

void set_backlight_duty(uint16_t duty)
{
    detail::backlight_set_duty(duty);
}

uint16_t backlight_max_duty()
{
    return detail::backlight_max_duty();
}

float backlight_level()
{
    return detail::backlight_level();
}

esp_err_t set_backlight_frequency(uint32_t hz)
{
    return detail::backlight_set_frequency(hz);
}

} // namespace hc::board
