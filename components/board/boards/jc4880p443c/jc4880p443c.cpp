// Guition JC4880P443C-I-W (Shenzhen Jingcai Intelligent): ESP32-P4, 16 MB flash, 32 MB PSRAM, an
// ESP32-C6 as the Wi-Fi radio (components/network), 4.3" 480x800 IPS panel with an ST7701S over
// MIPI-DSI (2 lanes), GT911 touch on I2C. No orientation or light sensor.
// Pins, timing and init table: docs/devices/JC4880P443C/JC4880P443C.md (from the vendor demo,
// github.com/wegi1/ESP32P4-JC4880P443C-I-W, 1-Demo).

#include "hc/board/board.hpp"

#include <string_view>

#include "esp_check.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7701.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"

#include "backlight.hpp"

// the board tag of every image built for this board (hc::board::ImageTag); CMakeLists.txt keeps it
extern "C" constexpr hc::board::ImageTag hc_image_tag __attribute__((section(".rodata_custom_desc"), used)) = {
    hc::board::kImageTagMagic, "JC4880P443C"};

namespace hc::board {

namespace {

constexpr const char *kTag = "board";

// The base orientation is the panel's own, portrait 480x800. A DPI panel scans a frame buffer and
// cannot rotate: LVGL rotates in software (Info::mipi_dsi).
constexpr Info kInfo{
    .name = "JC4880P443C",
    .width = 480,
    .height = 800,
    .has_touch = true,
    .swap_xy = false,
    .mirror_x = false,
    .mirror_y = false,
    .swap_bytes = false,
    // two draw buffers of 480 x 50 lines in internal RAM (47 KB each), copied into the frame
    // buffer in PSRAM by the CPU (init_display: not the DMA2D)
    .draw_buffer_lines = 50,
    .mipi_dsi = true,
    .imu_address = 0,
    // the default, frequency not compared yet
    .backlight_hz = 25000,
    // lowest readable duty, chosen by the user with the backlight tab (2026-10-08)
    .backlight_min_duty = 9,
    // no sensor built in
    .light_sensor_scale = 1.0f,
    .light_sensor_backlight_lux = 0.0f,
    // the large screen: pages slide, panels are dialogs, larger fonts (user, 2026-10-08)
    .ui_slide_pages = true,
    .ui_panel_dialogs = true,
    .ui_large_fonts = true,
    // twice the 4 px of the smaller screens (your choice on the device, 2026-10-09)
    .ui_tile_gap = 8,
};
// the image tag at the top of this file names the board the same way
static_assert(std::string_view(kInfo.name) == std::string_view(hc_image_tag.board));

constexpr uint16_t kNativeWidth = 480;
constexpr uint16_t kNativeHeight = 800;

// VDD_MIPI_DPHY: 2.5 V from the chip's LDO channel 3
constexpr int kPhyLdoChannel = 3;
constexpr int kPhyLdoMillivolts = 2500;

// MIPI-DSI: 2 data lanes at 500 Mbps; DPI pixel clock and video timing from the vendor demo
// (ST7701_480_360_PANEL_60HZ_DPI_CONFIG): 34 MHz / (480 + 12 + 42 + 42) / (800 + 2 + 8 + 166)
// = 60.5 Hz
constexpr int kDsiLanes = 2;
constexpr int kDsiLaneMbps = 500;
constexpr int kDpiClockMhz = 34;

constexpr gpio_num_t kPinReset = GPIO_NUM_5;
constexpr gpio_num_t kPinBacklight = GPIO_NUM_23;

// I2C bus of the touch controller; its RST and INT lines are not connected, so it is polled and
// its address is whichever of the two it took at power-up (the vendor demo uses 0x5D)
constexpr i2c_port_num_t kI2cPort = I2C_NUM_0;
constexpr gpio_num_t kPinSda = GPIO_NUM_7;
constexpr gpio_num_t kPinScl = GPIO_NUM_8;
// the vendor demo runs the GT911 at 100 kHz
constexpr uint32_t kI2cHz = 100000;

// Init sequence of this module: the vendor demo's esp_lcd_st7701_mipi.c (vendor_specific_init_default,
// which replaces the default of esp_lcd_st7701)
constexpr uint8_t kInit00[] = {0x77, 0x01, 0x00, 0x00, 0x13};
constexpr uint8_t kInit01[] = {0x08};
constexpr uint8_t kInit02[] = {0x77, 0x01, 0x00, 0x00, 0x10};
constexpr uint8_t kInit03[] = {0x63, 0x00};
constexpr uint8_t kInit04[] = {0x0D, 0x02};
constexpr uint8_t kInit05[] = {0x10, 0x08};
constexpr uint8_t kInit06[] = {0x10};
constexpr uint8_t kInit07[] = {0x80, 0x09, 0x53, 0x0C, 0xD0, 0x07, 0x0C, 0x09, 0x09, 0x28, 0x06, 0xD4, 0x13, 0x69, 0x2B, 0x71};
constexpr uint8_t kInit08[] = {0x80, 0x94, 0x5A, 0x10, 0xD3, 0x06, 0x0A, 0x08, 0x08, 0x25, 0x03, 0xD3, 0x12, 0x66, 0x6A, 0x0D};
constexpr uint8_t kInit09[] = {0x77, 0x01, 0x00, 0x00, 0x11};
constexpr uint8_t kInit10[] = {0x5D};
constexpr uint8_t kInit11[] = {0x58};
constexpr uint8_t kInit12[] = {0x87};
constexpr uint8_t kInit13[] = {0x80};
constexpr uint8_t kInit14[] = {0x4E};
constexpr uint8_t kInit15[] = {0x85};
constexpr uint8_t kInit16[] = {0x21};
constexpr uint8_t kInit17[] = {0x10, 0x1F};
constexpr uint8_t kInit18[] = {0x03};
constexpr uint8_t kInit19[] = {0x00};
constexpr uint8_t kInit20[] = {0x78};
constexpr uint8_t kInit21[] = {0x78};
constexpr uint8_t kInit22[] = {0x88};
constexpr uint8_t kInit23[] = {0x00, 0x3A, 0x02};
constexpr uint8_t kInit24[] = {0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0, 0x00, 0xA0, 0x00, 0x40, 0x40};
constexpr uint8_t kInit25[] = {0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00};
constexpr uint8_t kInit26[] = {0x00, 0x00, 0x33, 0x33};
constexpr uint8_t kInit27[] = {0x44, 0x44};
constexpr uint8_t kInit28[] = {0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0, 0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0};
constexpr uint8_t kInit29[] = {0x00, 0x00, 0x33, 0x33};
constexpr uint8_t kInit30[] = {0x44, 0x44};
constexpr uint8_t kInit31[] = {0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0, 0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0};
constexpr uint8_t kInit32[] = {0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00};
constexpr uint8_t kInit33[] = {0x08, 0x01};
constexpr uint8_t kInit34[] = {0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B};
constexpr uint8_t kInit35[] = {0x08, 0x08, 0x08, 0x45, 0x3F, 0x54};
constexpr uint8_t kInit36[] = {0x77, 0x01, 0x00, 0x00, 0x00};
constexpr uint8_t kInit37[] = {0x00};
constexpr uint8_t kInit38[] = {0x00};

constexpr st7701_lcd_init_cmd_t kInitCommands[] = {
    {0xFF, kInit00, sizeof(kInit00), 0},
    {0xEF, kInit01, sizeof(kInit01), 0},
    {0xFF, kInit02, sizeof(kInit02), 0},
    {0xC0, kInit03, sizeof(kInit03), 0},
    {0xC1, kInit04, sizeof(kInit04), 0},
    {0xC2, kInit05, sizeof(kInit05), 0},
    {0xCC, kInit06, sizeof(kInit06), 0},
    {0xB0, kInit07, sizeof(kInit07), 0},
    {0xB1, kInit08, sizeof(kInit08), 0},
    {0xFF, kInit09, sizeof(kInit09), 0},
    {0xB0, kInit10, sizeof(kInit10), 0},
    {0xB1, kInit11, sizeof(kInit11), 0},
    {0xB2, kInit12, sizeof(kInit12), 0},
    {0xB3, kInit13, sizeof(kInit13), 0},
    {0xB5, kInit14, sizeof(kInit14), 0},
    {0xB7, kInit15, sizeof(kInit15), 0},
    {0xB8, kInit16, sizeof(kInit16), 0},
    {0xB9, kInit17, sizeof(kInit17), 0},
    {0xBB, kInit18, sizeof(kInit18), 0},
    {0xBC, kInit19, sizeof(kInit19), 0},
    {0xC1, kInit20, sizeof(kInit20), 0},
    {0xC2, kInit21, sizeof(kInit21), 0},
    {0xD0, kInit22, sizeof(kInit22), 0},
    {0xE0, kInit23, sizeof(kInit23), 0},
    {0xE1, kInit24, sizeof(kInit24), 0},
    {0xE2, kInit25, sizeof(kInit25), 0},
    {0xE3, kInit26, sizeof(kInit26), 0},
    {0xE4, kInit27, sizeof(kInit27), 0},
    {0xE5, kInit28, sizeof(kInit28), 0},
    {0xE6, kInit29, sizeof(kInit29), 0},
    {0xE7, kInit30, sizeof(kInit30), 0},
    {0xE8, kInit31, sizeof(kInit31), 0},
    {0xEB, kInit32, sizeof(kInit32), 0},
    {0xEC, kInit33, sizeof(kInit33), 0},
    {0xED, kInit34, sizeof(kInit34), 0},
    {0xEF, kInit35, sizeof(kInit35), 0},
    {0xFF, kInit36, sizeof(kInit36), 0},
    {0x11, kInit37, sizeof(kInit37), 120},
    {0x29, kInit38, sizeof(kInit38), 20},
};

esp_err_t init_panel(Handles &out)
{
    // the PHY needs its supply before the bus is created; the channel stays acquired
    esp_ldo_channel_handle_t phy_ldo = nullptr;
    esp_ldo_channel_config_t ldo_config{};
    ldo_config.chan_id = kPhyLdoChannel;
    ldo_config.voltage_mv = kPhyLdoMillivolts;
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_config, &phy_ldo), kTag, "DSI PHY supply");

    esp_lcd_dsi_bus_config_t bus_config{};
    bus_config.bus_id = 0;
    bus_config.num_data_lanes = kDsiLanes;
    bus_config.phy_clk_src = MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT;
    bus_config.lane_bit_rate_mbps = kDsiLaneMbps;
    esp_lcd_dsi_bus_handle_t bus = nullptr;
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &bus), kTag, "DSI bus");

    // commands go over the DBI (generic) interface of the same bus
    esp_lcd_dbi_io_config_t io_config{};
    io_config.virtual_channel = 0;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(bus, &io_config, &out.io), kTag, "panel IO");

    // one frame buffer in PSRAM, which the DPI controller scans continuously
    esp_lcd_dpi_panel_config_t dpi_config{};
    dpi_config.virtual_channel = 0;
    dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_config.dpi_clock_freq_mhz = kDpiClockMhz;
    dpi_config.in_color_format = LCD_COLOR_FMT_RGB565;
    dpi_config.num_fbs = 1;
    dpi_config.video_timing.h_size = kNativeWidth;
    dpi_config.video_timing.v_size = kNativeHeight;
    dpi_config.video_timing.hsync_pulse_width = 12;
    dpi_config.video_timing.hsync_back_porch = 42;
    dpi_config.video_timing.hsync_front_porch = 42;
    dpi_config.video_timing.vsync_pulse_width = 2;
    dpi_config.video_timing.vsync_back_porch = 8;
    dpi_config.video_timing.vsync_front_porch = 166;

    st7701_vendor_config_t vendor_config{};
    vendor_config.init_cmds = kInitCommands;
    vendor_config.init_cmds_size = sizeof(kInitCommands) / sizeof(kInitCommands[0]);
    vendor_config.mipi_config.dsi_bus = bus;
    vendor_config.mipi_config.dpi_config = &dpi_config;
    vendor_config.flags.use_mipi_interface = 1;
    esp_lcd_panel_dev_config_t panel_config{};
    panel_config.reset_gpio_num = kPinReset;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = &vendor_config;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7701(out.io, &panel_config, &out.panel), kTag, "ST7701");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(out.panel), kTag, "panel reset");
    // the table ends with sleep out and display on (11h, 29h), then the DPI video stream starts
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(out.panel), kTag, "panel init");
    // draw_bitmap copies LVGL's bands into the frame buffer with the CPU. With the DMA2D
    // (esp_lcd_dpi_panel_enable_dma2d) memory got corrupted while a camera image was shown full
    // screen: 4 of 4 stream starts crashed (heap, lwIP and FreeRTOS lists), without it 0 of 10
    // (2026-10-08, the cause inside the DMA2D path not found).
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

esp_err_t init_touch(Handles &out, uint8_t &address)
{
    address = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    if (i2c_master_probe(out.i2c, address, 50) != ESP_OK) {
        address = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
        ESP_RETURN_ON_ERROR(i2c_master_probe(out.i2c, address, 50), kTag, "no GT911 at 0x5D or 0x14");
    }

    // ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG, which is C only
    esp_lcd_panel_io_i2c_config_t io_config{};
    io_config.dev_addr = address;
    io_config.control_phase_bytes = 1;
    io_config.dc_bit_offset = 0;
    io_config.lcd_cmd_bits = 16;
    io_config.flags.disable_control_phase = 1;
    io_config.scl_speed_hz = kI2cHz;
    // read from the LVGL task: never wait forever on the bus (see the WT32-SC01)
    io_config.transaction_timeout_ms = 50;
    esp_lcd_panel_io_handle_t io = nullptr;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(out.i2c, &io_config, &io), kTag, "touch IO");

    // The driver needs the address it was found at when RST/INT cannot select it
    esp_lcd_touch_io_gt911_config_t gt911_config{};
    gt911_config.dev_addr = address;
    // The controller reports the panel's coordinates, which are those of the base orientation
    esp_lcd_touch_config_t config{};
    config.x_max = kNativeWidth;
    config.y_max = kNativeHeight;
    config.rst_gpio_num = GPIO_NUM_NC;
    config.int_gpio_num = GPIO_NUM_NC;
    config.driver_data = &gt911_config;
    return esp_lcd_touch_new_i2c_gt911(io, &config, &out.touch);
}

} // namespace

const Info &info()
{
    return kInfo;
}

esp_err_t init(Handles &out)
{
    out = {};
    ESP_RETURN_ON_ERROR(detail::backlight_init(kPinBacklight, kInfo.backlight_hz, kInfo.backlight_min_duty), kTag, "backlight");
    ESP_RETURN_ON_ERROR(init_panel(out), kTag, "panel");
    ESP_LOGI(kTag, "%s: ST7701S %ux%u, MIPI-DSI %d lanes at %d Mbps, pixel clock %d MHz", kInfo.name, kNativeWidth,
             kNativeHeight, kDsiLanes, kDsiLaneMbps, kDpiClockMhz);

    if (init_i2c(out) != ESP_OK) {
        ESP_LOGW(kTag, "I2C bus failed, no touch");
        out.i2c = nullptr;
        return ESP_OK;
    }
    uint8_t address = 0;
    if (init_touch(out, address) == ESP_OK) {
        ESP_LOGI(kTag, "GT911 touch at 0x%02x, polled", address);
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
