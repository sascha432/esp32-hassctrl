// LilyGO TTGO T4 v1.3: ESP32-WROVER-B, 4 MB flash, 8 MB PSRAM, 2.4" 240x320 ILI9341 over SPI,
// no touch. A test device only: touch is a requirement for supported boards. The I2C bus of the
// IP5306 power chip (SDA 21, SCL 22) takes optional sensor modules: an MPU6050 at 0x68 and a
// BH1750 (on the unit used here since 2026-10-07).
// Pins: docs/devices/TTGO_T4/TTGO_T4.md. Panel settings verified with the previous attempt.

#include "hc/board/board.hpp"

#include <string_view>

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

#include "backlight.hpp"
#include "hc_board_variant.hpp"

// the board tag of every image built for this board (hc::board::ImageTag); CMakeLists.txt keeps it
extern "C" constexpr hc::board::ImageTag hc_image_tag __attribute__((section(".rodata_custom_desc"), used)) = {
    hc::board::kImageTagMagic, "TTGO T4"};

namespace hc::board {

namespace {

constexpr const char *kTag = "board";

// a unit's add-ons go into a variant (hc_board_variant.hpp)
constexpr Info kInfo = variant::apply(Info{
    .name = "TTGO T4",
    .width = 240,
    .height = 320,
    .has_touch = false,
    .swap_xy = false,
    .mirror_x = true,
    .mirror_y = false,
    .swap_bytes = true,
    .draw_buffer_lines = 40,
    // optional module on the IP5306 bus; without it the orientation component finds nothing
    .imu_address = 0x68,
    // not measured on the T4, the default
    .backlight_hz = 25000,
    // not measured yet
    .backlight_min_duty = 0,
    // no sensor built in: a module on a cable sees the room as it is
    .light_sensor_scale = 1.0f,
    .light_sensor_backlight_lux = 0.0f,
});
// the image tag at the top of this file names the board the same way
static_assert(std::string_view(kInfo.name) == std::string_view(hc_image_tag.board));

constexpr spi_host_device_t kSpiHost = SPI2_HOST;
constexpr int kPixelClockHz = 40 * 1000 * 1000;
// largest single transfer: one draw buffer of the longest side, in case the screen is rotated
constexpr int kMaxTransferBytes = kInfo.height * kInfo.draw_buffer_lines * 2;

constexpr gpio_num_t kPinSclk = GPIO_NUM_18;
constexpr gpio_num_t kPinMosi = GPIO_NUM_23;
constexpr gpio_num_t kPinMiso = GPIO_NUM_12;
constexpr gpio_num_t kPinCs = GPIO_NUM_27;
constexpr gpio_num_t kPinDc = GPIO_NUM_32;
constexpr gpio_num_t kPinReset = GPIO_NUM_5;
constexpr gpio_num_t kPinBacklight = GPIO_NUM_4;

// the IP5306's I2C bus (0x75); sensor modules bring their own pull-ups
constexpr i2c_port_num_t kI2cPort = I2C_NUM_0;
constexpr gpio_num_t kPinSda = GPIO_NUM_21;
constexpr gpio_num_t kPinScl = GPIO_NUM_22;

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

    spi_bus_config_t bus{};
    bus.sclk_io_num = kPinSclk;
    bus.mosi_io_num = kPinMosi;
    bus.miso_io_num = kPinMiso;
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
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9341(out.io, &panel_config, &out.panel), kTag, "ILI9341");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(out.panel), kTag, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(out.panel), kTag, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(out.panel, kInfo.swap_xy), kTag, "swap_xy");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(out.panel, kInfo.mirror_x, kInfo.mirror_y), kTag, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(out.panel, false), kTag, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(out.panel, true), kTag, "display on");
    detail::set_restart_panel(out.panel);

    ESP_LOGI(kTag, "%s: ILI9341 %ux%u, SPI %d MHz", kInfo.name, kInfo.width, kInfo.height, kPixelClockHz / 1000000);

    if (init_i2c(out) != ESP_OK) {
        ESP_LOGW(kTag, "I2C bus failed, no sensors");
        out.i2c = nullptr;
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
