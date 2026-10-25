// Guition JC3248W535 (Shenzhen Jingcai Intelligent): ESP32-S3, 16 MB flash, 8 MB octal PSRAM,
// 3.5" 320x480 IPS panel with an AXS15231B over QSPI (write only, no reset line), the touch
// controller of the same chip on I2C, an NS4168 I2S amplifier for a speaker. No orientation or
// light sensor.
// Pins: docs/devices/JC3248W535/JC3248W535.md (from ESPHome, Arduino_GFX and the vendor demo).

#include "hc/board/board.hpp"

#include <string_view>

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

#include "backlight.hpp"
#include "hc_board_variant.hpp"

// the board tag of every image built for this board (hc::board::ImageTag); CMakeLists.txt keeps it
extern "C" constexpr hc::board::ImageTag hc_image_tag __attribute__((section(".rodata_custom_desc"), used)) = {
    hc::board::kImageTagMagic, "JC3248W535"};

namespace hc::board {

namespace {

constexpr const char *kTag = "board";

// The base orientation is the panel's own, portrait 320x480: no row/column exchange, which the
// controller would have to do while it takes the whole frame (to be checked, adding-a-board step 4).
// A unit's add-ons go into a variant (hc_board_variant.hpp).
constexpr Info kInfo = variant::apply(Info{
    .name = "JC3248W535",
    .width = 320,
    .height = 480,
    .has_touch = true,
    .swap_xy = false,
    .mirror_x = false,
    .mirror_y = false,
    .swap_bytes = true,
    // the frame is sent in bands of this many lines, two band buffers of 20 KB in internal DMA RAM
    // (16 lines would save 20 KB but cost 3-5 % per full frame, benchmark 2026-10-25)
    .draw_buffer_lines = 32,
    .full_frames = true,
    // the touch reports at 60 Hz, a read in between answers "no finger" (docs/devices/JC3248W535)
    .touch_sample_ms = 20,
    .imu_address = 0,
    // the default, no difference reported
    .backlight_hz = 25000,
    // lowest readable duty, chosen by the user with the backlight tab (2026-10-08)
    .backlight_min_duty = 3,
    // no sensor built in
    .light_sensor_scale = 1.0f,
    .light_sensor_backlight_lux = 0.0f,
    // the NS4168 amplifier of the speaker connector (docs/devices/JC3248W535)
    .audio_bclk_gpio = 42,
    .audio_ws_gpio = 2,
    .audio_dout_gpio = 41,
    // a tap about a second after the last sound caught the NS4168 waking or going to sleep: that
    // click was quieter (2026-10-09)
    .audio_always_on = true,
#if CONFIG_HC_SDCARD
    // the TF slot (schematic, wired for SPI: CS 10, MOSI 11, CLK 12, MISO 13, 10k pullups, 3.3 V
    // rail, no card detect): SDMMC 1 bit through the GPIO matrix, CMD on MOSI, D0 on MISO; D1 and
    // D2 are not connected, D3 (CS) is held high so the card stays in SD mode
    .sd_clk_gpio = 12,
    .sd_cmd_gpio = 11,
    .sd_data_gpios = {13, -1, -1, 10},
#endif
});
// the image tag at the top of this file names the board the same way
static_assert(std::string_view(kInfo.name) == std::string_view(hc_image_tag.board));

constexpr uint16_t kNativeWidth = 320;
constexpr uint16_t kNativeHeight = 480;

// QSPI through the GPIO matrix (the pins are not SPI2's IO MUX pins). 40 MHz, measured on the
// camera 2026-10-08: the datasheet allows 90 MHz (TSCYCW 11 ns), but 80 MHz garbled the picture
// and 60 MHz is 40 MHz (the clock is 80 MHz divided by a whole number). Arduino_GFX uses 32 MHz.
constexpr spi_host_device_t kSpiHost = SPI2_HOST;
constexpr int kPixelClockHz = 40 * 1000 * 1000;
// A band larger than this is sent in pieces with CS held (esp_lcd splits it); 50 lines stay
// below the 32 KB of one SPI DMA transfer on the ESP32-S3.
constexpr int kMaxTransferBytes = kNativeWidth * 50 * 2;

constexpr gpio_num_t kPinCs = GPIO_NUM_45;
constexpr gpio_num_t kPinSclk = GPIO_NUM_47;
constexpr gpio_num_t kPinData0 = GPIO_NUM_21;
constexpr gpio_num_t kPinData1 = GPIO_NUM_48;
constexpr gpio_num_t kPinData2 = GPIO_NUM_40;
constexpr gpio_num_t kPinData3 = GPIO_NUM_39;
constexpr gpio_num_t kPinBacklight = GPIO_NUM_1;

// I2C bus of the touch controller (0x3B); INT is GPIO 3, the driver polls like on the WT32-SC01
constexpr i2c_port_num_t kI2cPort = I2C_NUM_0;
constexpr gpio_num_t kPinSda = GPIO_NUM_4;
constexpr gpio_num_t kPinScl = GPIO_NUM_8;
constexpr uint32_t kI2cHz = 400000;

// Init sequence of this module: Arduino_GFX 1.6.0, axs15231b_init_operations
// (src/display/Arduino_AXS15231B.h), used by the working demo of the module
// (github.com/F1ATB/JC3248W535-Demo). The default of esp_lcd_axs15231b is for another panel and
// ends with "all pixels off" (22h).
constexpr uint8_t kInit00[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0xA5};
constexpr uint8_t kInit01[] = {0xC0, 0x10, 0x00, 0x02, 0x00, 0x00, 0x04, 0x3F, 0x20, 0x05, 0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr uint8_t kInit02[] = {0x30, 0x3C, 0x24, 0x14, 0xD0, 0x20, 0xFF, 0xE0, 0x40, 0x19, 0x80, 0x80, 0x80, 0x20, 0xF9, 0x10, 0x02, 0xFF, 0xFF, 0xF0, 0x90, 0x01, 0x32, 0xA0, 0x91, 0xE0, 0x20, 0x7F, 0xFF, 0x00, 0x5A};
constexpr uint8_t kInit03[] = {0xE0, 0x40, 0x51, 0x24, 0x08, 0x05, 0x10, 0x01, 0x20, 0x15, 0xC2, 0x42, 0x22, 0x22, 0xAA, 0x03, 0x10, 0x12, 0x60, 0x14, 0x1E, 0x51, 0x15, 0x00, 0x8A, 0x20, 0x00, 0x03, 0x3A, 0x12};
constexpr uint8_t kInit04[] = {0xA0, 0x06, 0xAA, 0x00, 0x08, 0x02, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x55, 0x55};
constexpr uint8_t kInit05[] = {0x31, 0x04, 0x02, 0x02, 0x71, 0x05, 0x24, 0x55, 0x02, 0x00, 0x41, 0x00, 0x53, 0xFF, 0xFF, 0xFF, 0x4F, 0x52, 0x00, 0x4F, 0x52, 0x00, 0x45, 0x3B, 0x0B, 0x02, 0x0D, 0x00, 0xFF, 0x40};
constexpr uint8_t kInit06[] = {0x00, 0x00, 0x00, 0x50, 0x03, 0x00, 0x00, 0x00, 0x01, 0x80, 0x01};
constexpr uint8_t kInit07[] = {0x00, 0x24, 0x33, 0x80, 0x00, 0xEA, 0x64, 0x32, 0xC8, 0x64, 0xC8, 0x32, 0x90, 0x90, 0x11, 0x06, 0xDC, 0xFA, 0x00, 0x00, 0x80, 0xFE, 0x10, 0x10, 0x00, 0x0A, 0x0A, 0x44, 0x50};
constexpr uint8_t kInit08[] = {0x18, 0x00, 0x00, 0x03, 0xFE, 0x3A, 0x4A, 0x20, 0x30, 0x10, 0x88, 0xDE, 0x0D, 0x08, 0x0F, 0x0F, 0x01, 0x3A, 0x4A, 0x20, 0x10, 0x10, 0x00};
constexpr uint8_t kInit09[] = {0x05, 0x0A, 0x05, 0x0A, 0x00, 0xE0, 0x2E, 0x0B, 0x12, 0x22, 0x12, 0x22, 0x01, 0x03, 0x00, 0x3F, 0x6A, 0x18, 0xC8, 0x22};
constexpr uint8_t kInit10[] = {0x50, 0x32, 0x28, 0x00, 0xA2, 0x80, 0x8F, 0x00, 0x80, 0xFF, 0x07, 0x11, 0x9C, 0x67, 0xFF, 0x24, 0x0C, 0x0D, 0x0E, 0x0F};
constexpr uint8_t kInit11[] = {0x33, 0x44, 0x44, 0x01};
constexpr uint8_t kInit12[] = {0x2C, 0x1E, 0x88, 0x58, 0x13, 0x18, 0x56, 0x18, 0x1E, 0x68, 0x88, 0x00, 0x65, 0x09, 0x22, 0xC4, 0x0C, 0x77, 0x22, 0x44, 0xAA, 0x55, 0x08, 0x08, 0x12, 0xA0, 0x08};
constexpr uint8_t kInit13[] = {0x40, 0x8E, 0x8D, 0x01, 0x35, 0x04, 0x92, 0x74, 0x04, 0x92, 0x74, 0x04, 0x08, 0x6A, 0x04, 0x46, 0x03, 0x03, 0x03, 0x03, 0x82, 0x01, 0x03, 0x00, 0xE0, 0x51, 0xA1, 0x00, 0x00, 0x00};
constexpr uint8_t kInit14[] = {0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x93, 0x00, 0x01, 0x83, 0x07, 0x07, 0x00, 0x07, 0x07, 0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x00, 0x84, 0x00, 0x20, 0x01, 0x00};
constexpr uint8_t kInit15[] = {0x03, 0x01, 0x0B, 0x09, 0x0F, 0x0D, 0x1E, 0x1F, 0x18, 0x1D, 0x1F, 0x19, 0x40, 0x8E, 0x04, 0x00, 0x20, 0xA0, 0x1F};
constexpr uint8_t kInit16[] = {0x02, 0x00, 0x0A, 0x08, 0x0E, 0x0C, 0x1E, 0x1F, 0x18, 0x1D, 0x1F, 0x19};
constexpr uint8_t kInit17[] = {0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F};
constexpr uint8_t kInit18[] = {0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F};
constexpr uint8_t kInit19[] = {0x44, 0x73, 0x4B, 0x69, 0x00, 0x0A, 0x02, 0x90};
constexpr uint8_t kInit20[] = {0x3B, 0x28, 0x10, 0x16, 0x0C, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x13, 0x2C, 0x33, 0x28, 0x0D};
constexpr uint8_t kInit21[] = {0x37, 0x28, 0x10, 0x16, 0x0B, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x14, 0x2C, 0x33, 0x28, 0x0F};
constexpr uint8_t kInit22[] = {0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D};
constexpr uint8_t kInit23[] = {0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x32, 0x2F, 0x0F};
constexpr uint8_t kInit24[] = {0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D};
constexpr uint8_t kInit25[] = {0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0F};
constexpr uint8_t kInit26[] = {0x85, 0x85, 0x95, 0x82, 0xAF, 0xAA, 0xAA, 0x80, 0x10, 0x30, 0x40, 0x40, 0x20, 0xFF, 0x60, 0x30};
constexpr uint8_t kInit27[] = {0x85, 0x85, 0x95, 0x85};
constexpr uint8_t kInit28[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr uint8_t kInit29[] = {0x00};
constexpr uint8_t kInit30[] = {0x00};
constexpr uint8_t kInit31[] = {0x00, 0x00, 0x00, 0x00};

constexpr axs15231b_lcd_init_cmd_t kInitCommands[] = {
    {0xBB, kInit00, sizeof(kInit00), 0},
    {0xA0, kInit01, sizeof(kInit01), 0},
    {0xA2, kInit02, sizeof(kInit02), 0},
    {0xD0, kInit03, sizeof(kInit03), 0},
    {0xA3, kInit04, sizeof(kInit04), 0},
    {0xC1, kInit05, sizeof(kInit05), 0},
    {0xC3, kInit06, sizeof(kInit06), 0},
    {0xC4, kInit07, sizeof(kInit07), 0},
    {0xC5, kInit08, sizeof(kInit08), 0},
    {0xC6, kInit09, sizeof(kInit09), 0},
    {0xC7, kInit10, sizeof(kInit10), 0},
    {0xC9, kInit11, sizeof(kInit11), 0},
    {0xCF, kInit12, sizeof(kInit12), 0},
    {0xD5, kInit13, sizeof(kInit13), 0},
    {0xD6, kInit14, sizeof(kInit14), 0},
    {0xD7, kInit15, sizeof(kInit15), 0},
    {0xD8, kInit16, sizeof(kInit16), 0},
    {0xD9, kInit17, sizeof(kInit17), 0},
    {0xDD, kInit18, sizeof(kInit18), 0},
    {0xDF, kInit19, sizeof(kInit19), 0},
    {0xE0, kInit20, sizeof(kInit20), 0},
    {0xE1, kInit21, sizeof(kInit21), 0},
    {0xE2, kInit22, sizeof(kInit22), 0},
    {0xE3, kInit23, sizeof(kInit23), 0},
    {0xE4, kInit24, sizeof(kInit24), 0},
    {0xE5, kInit25, sizeof(kInit25), 0},
    {0xA4, kInit26, sizeof(kInit26), 0},
    {0xA4, kInit27, sizeof(kInit27), 0},
    {0xBB, kInit28, sizeof(kInit28), 0},
    {0x11, kInit29, sizeof(kInit29), 120},
    {0x29, kInit30, sizeof(kInit30), 100},
    {0x2C, kInit31, sizeof(kInit31), 0},
};

esp_err_t init_panel(Handles &out)
{
    spi_bus_config_t bus{};
    bus.sclk_io_num = kPinSclk;
    bus.data0_io_num = kPinData0;
    bus.data1_io_num = kPinData1;
    bus.data2_io_num = kPinData2;
    bus.data3_io_num = kPinData3;
    bus.max_transfer_sz = kMaxTransferBytes;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(kSpiHost, &bus, SPI_DMA_CH_AUTO), kTag, "QSPI bus");

    // QSPI framing of the AXS15231B: a 32 bit command (opcode, command, 0) and no DC line
    esp_lcd_panel_io_spi_config_t io_config{};
    io_config.cs_gpio_num = kPinCs;
    io_config.dc_gpio_num = GPIO_NUM_NC;
    io_config.spi_mode = 0; // as Arduino_GFX
    io_config.pclk_hz = kPixelClockHz;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 32;
    io_config.lcd_param_bits = 8;
    io_config.flags.quad_mode = true;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kSpiHost), &io_config, &out.io), kTag,
        "panel IO");

    axs15231b_vendor_config_t vendor_config{};
    vendor_config.init_cmds = kInitCommands;
    vendor_config.init_cmds_size = sizeof(kInitCommands) / sizeof(kInitCommands[0]);
    vendor_config.flags.use_qspi_interface = 1;
    esp_lcd_panel_dev_config_t panel_config{};
    panel_config.reset_gpio_num = GPIO_NUM_NC; // software reset
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = &vendor_config;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_axs15231b(out.io, &panel_config, &out.panel), kTag, "AXS15231B");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(out.panel), kTag, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(out.panel), kTag, "panel init");
    // No esp_lcd_panel_disp_on_off(): the table switches the display on (29h with a zero byte).
    // The driver's 29h without a parameter left the screen black, whatever came before it
    // (2026-10-08, seen on a camera). Rotation comes from esp_lvgl_port when LVGL starts.
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

// The touch answer of the AXS15231B: gesture, count of fingers, then 6 bytes per finger (x high
// nibble with the event in bits 6-7, x low, y high nibble with the finger's id in bits 4-7, y low,
// two bytes that count up by one). Asked for two fingers (pinch). Without a finger the header is
// anything else (0x30 throughout after a lift). About one answer in 40 during a touch came with a
// good header but records shifted by a few bytes (2026-10-11, 330 good records): every good
// record's last two bytes counted up by one and no shifted one's did, so such an answer is a
// failed read (hc::touch_input keeps the last sample), not a lift or a touch somewhere else.
constexpr uint8_t kTouchFingers = 2;
// A new report comes about every 17 to 22 ms, a read without one answers "no finger" (read every
// 10 ms a held finger went 11.11.1.11). Read every 20 ms a touch still has a single such answer
// now and then (2026-10-17: 7 in 3 minutes of use, 13 in fast tapping), never two in a row
// during a hold or a drag: a lift counts from the second answer without a finger, the first is a
// misread. Tapping faster than a lift of 40 ms can merge two taps.
bool g_touch_down = false;
bool g_touch_gap = false;

esp_err_t read_touch(esp_lcd_touch_handle_t tp)
{
    uint8_t data[2 + kTouchFingers * 6]{};
    const uint8_t command[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, 0x00, sizeof(data), 0x00, 0x00, 0x00};
    esp_err_t err = esp_lcd_panel_io_tx_param(tp->io, -1, command, sizeof(command));
    if (err == ESP_OK) {
        err = esp_lcd_panel_io_rx_param(tp->io, -1, data, sizeof(data));
    }
    if (err != ESP_OK) {
        return err;
    }
    // no finger: the controller answers anything after a lift (0x30 throughout, ...), and the
    // driver took every answer it did not understand for none
    const uint8_t count = data[0] == 0 && data[1] <= kTouchFingers ? data[1] : 0;
    if (count == 0 && g_touch_down && !g_touch_gap) {
        g_touch_gap = true;
        return ESP_ERR_INVALID_RESPONSE;
    }
    g_touch_gap = false;
    g_touch_down = count != 0;
    esp_lcd_touch_point_data_t points[kTouchFingers]{};
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *record = data + 2 + i * 6;
        const uint16_t x = static_cast<uint16_t>((record[0] & 0x0f) << 8 | record[1]);
        const uint16_t y = static_cast<uint16_t>((record[2] & 0x0f) << 8 | record[3]);
        if (x > tp->config.x_max || y > tp->config.y_max || record[5] != static_cast<uint8_t>(record[4] + 1)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        points[i].track_id = record[2] >> 4;
        points[i].x = x;
        points[i].y = y;
    }
    portENTER_CRITICAL(&tp->data.lock);
    tp->data.points = count;
    for (uint8_t i = 0; i < count; i++) {
        tp->data.coords[i] = points[i];
    }
    portEXIT_CRITICAL(&tp->data.lock);
    return ESP_OK;
}

// the driver has none: the ids read_touch keeps with the points
esp_err_t touch_track_ids(esp_lcd_touch_handle_t tp, uint8_t *track_id, uint8_t point_num)
{
    portENTER_CRITICAL(&tp->data.lock);
    for (uint8_t i = 0; i < point_num && i < kTouchFingers; i++) {
        track_id[i] = tp->data.coords[i].track_id;
    }
    portEXIT_CRITICAL(&tp->data.lock);
    return ESP_OK;
}

esp_err_t init_touch(Handles &out)
{
    // ESP_LCD_TOUCH_IO_I2C_AXS15231B_CONFIG_EX, which is C only
    esp_lcd_panel_io_i2c_config_t io_config{};
    io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_AXS15231B_ADDRESS;
    io_config.control_phase_bytes = 1;
    io_config.dc_bit_offset = 0;
    io_config.lcd_cmd_bits = 8;
    io_config.flags.disable_control_phase = 1;
    io_config.scl_speed_hz = kI2cHz;
    // read from the LVGL task: never wait forever on the bus (see the WT32-SC01)
    io_config.transaction_timeout_ms = 50;
    esp_lcd_panel_io_handle_t io = nullptr;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(out.i2c, &io_config, &io), kTag, "touch IO");

    // The controller reports the panel's coordinates, which are those of the base orientation
    esp_lcd_touch_config_t config{};
    config.x_max = kNativeWidth - 1;
    config.y_max = kNativeHeight - 1;
    config.rst_gpio_num = GPIO_NUM_NC;
    config.int_gpio_num = GPIO_NUM_NC;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_axs15231b(io, &config, &out.touch), kTag, "touch");
    // the driver's read asks for one finger and drops a touch of two
    out.touch->read_data = read_touch;
    out.touch->get_track_id = touch_track_ids;
    return ESP_OK;
}

} // namespace

const Info &info()
{
    return kInfo;
}

const AudioCodec *audio_codec()
{
    return nullptr; // the amplifier takes the I2S data as it is
}

esp_err_t init(Handles &out)
{
    out = {};
    ESP_RETURN_ON_ERROR(detail::backlight_init(kPinBacklight, kInfo.backlight_hz, kInfo.backlight_min_duty), kTag, "backlight");
    ESP_RETURN_ON_ERROR(init_panel(out), kTag, "panel");
    detail::set_restart_panel(out.panel);
    ESP_LOGI(kTag, "%s: AXS15231B %ux%u, QSPI %d MHz", kInfo.name, kNativeWidth, kNativeHeight, kPixelClockHz / 1000000);

    if (init_i2c(out) != ESP_OK) {
        ESP_LOGW(kTag, "I2C bus failed, no touch");
        out.i2c = nullptr;
        return ESP_OK;
    }
    if (init_touch(out) == ESP_OK) {
        ESP_LOGI(kTag, "AXS15231B touch at 0x%02x, polled", ESP_LCD_TOUCH_IO_I2C_AXS15231B_ADDRESS);
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
