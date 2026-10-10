// The recovery app: runs from the factory partition when the main firmware is being updated on a
// 4 MB board, is broken, or the user asks for it (docs/architecture.md section 9.5).
//
// Same storage, Wi-Fi, hotspot and web server (with its login) as the main firmware, from the
// same NVS: the web page offers the firmware and file system update and "Start the firmware".
// No LVGL. The screen shows a few status lines and a QR code of the web address, drawn straight to
// the panel in bands (see "The screen" below).

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hc/board/board.hpp"
#include "hc/network/network.hpp"
#include "hc/ota/ota.hpp"
#include "hc/platform/device.hpp"
#include "hc/platform/storage.hpp"
#include "hc/platform/system_info.hpp"
#include "hc/webserver/webserver.hpp"
#include "qrcodegen.h"

namespace {
constexpr const char *kTag = "recovery";

// ---- The screen ------------------------------------------------------------------------------
// A page is a list of text lines and one QR code. It is drawn top to bottom in bands of
// Info::draw_buffer_lines rows, every band computed from the list, so no frame buffer is needed
// and it works for every panel type (SPI panels, the AXS15231B that only takes whole frames from
// the top, the MIPI-DSI panel). The text is the 8x8 font of recovery/font8x8.inc.

constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kWhite = 0xffff;
constexpr uint16_t kCyan = 0x07ff;
constexpr uint16_t kRed = 0xf800;
constexpr uint16_t kGreen = 0x07e0;
constexpr uint16_t kGrey = 0xad55;

constexpr uint8_t kFont[95][8] = {
#include "font8x8.inc"
};

constexpr int kMargin = 8;
constexpr int kMaxLines = 12;
constexpr int kQuietModules = 2;
// version 10 holds 174 bytes at the lowest error correction: far more than an address
constexpr int kQrMaxVersion = 10;

struct Line {
    char text[48];
    int x, y;
    uint8_t scale;
    uint16_t color;
};

struct Page {
    Line lines[kMaxLines];
    int line_count;
    // the QR code: its card (modules plus quiet zone) at qr_x/qr_y, module_px pixels per module
    bool has_qr;
    int qr_x, qr_y, module_px;
};

Page g_page;
uint8_t g_qr[qrcodegen_BUFFER_LEN_FOR_VERSION(kQrMaxVersion)];
// the address behind the QR code, kept to see when the page has to be drawn again
char g_drawn_signature[512];

struct Screen {
    hc::board::Handles handles;
    const hc::board::Info *info;
    uint16_t *bands[2];
    bool ok;
} g_screen;

int text_width(const char *text, int scale)
{
    return static_cast<int>(std::strlen(text)) * 8 * scale;
}

void add_line(const char *text, int x, int y, int scale, uint16_t color)
{
    if (g_page.line_count >= kMaxLines) {
        return;
    }
    Line &line = g_page.lines[g_page.line_count++];
    std::snprintf(line.text, sizeof(line.text), "%s", text);
    line.x = x;
    line.y = y;
    line.scale = static_cast<uint8_t>(scale);
    line.color = color;
}

// A line cut to `chars` characters, with ".." at the end when it was longer
void fit(char *text, size_t chars)
{
    if (std::strlen(text) > chars && chars > 2) {
        text[chars - 2] = '.';
        text[chars - 1] = '.';
        text[chars] = 0;
    }
}

// Lines with printf formatting, cut to the width of their column
void add_text(int &y, int x, int width, int scale, uint16_t color, const char *format, ...) __attribute__((format(printf, 6, 7)));

void add_text(int &y, int x, int width, int scale, uint16_t color, const char *format, ...)
{
    char text[48];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    fit(text, std::min<size_t>(sizeof(text) - 1, static_cast<size_t>(std::max(0, width / (8 * scale)))));
    add_line(text, x, y, scale, color);
    y += 8 * scale + std::max(2, scale * 2);
}

// The Wi-Fi QR code format: WIFI:T:WPA;S:<ssid>;P:<password>;; with \ ; , : " escaped
void wifi_join_text(char *out, size_t size, const char *ssid, const char *password)
{
    auto escape = [](char *o, size_t room, const char *in) {
        size_t n = 0;
        for (; *in != 0 && n + 2 < room; in++) {
            if (std::strchr("\\;,:\"", *in) != nullptr) {
                o[n++] = '\\';
            }
            o[n++] = *in;
        }
        o[n] = 0;
    };
    char s[2 * hc::network::kSsidSize];
    char p[2 * 12];
    escape(s, sizeof(s), ssid);
    escape(p, sizeof(p), password);
    std::snprintf(out, size, "WIFI:T:WPA;S:%s;P:%s;;", s, p);
}

// Builds the page for the current state. Landscape: the text on the left, the QR code and the
// address under it on the right. Portrait: the text on top, the QR code and the address under it.
void build_page()
{
    const hc::board::Info &info = *g_screen.info;
    const int w = info.width;
    const int h = info.height;
    const bool landscape = w > h;
    const hc::ota::Info ota = hc::ota::info();
    const hc::network::Status net = hc::network::status();

    g_page = {};
    const int scale = (w >= 480 && !landscape) ? 2 : 1;
    const int text_x = kMargin;
    const int text_w = landscape ? w * 45 / 100 - kMargin : w - 2 * kMargin;

    int y = kMargin;
    add_text(y, text_x, text_w, scale + 1, kCyan, "RECOVERY");
    y += 4;
    add_text(y, text_x, text_w, scale, kGrey, "%s", hc::board::info().name);
    if (ota.firmware_failed) {
        add_text(y, text_x, text_w, scale, kRed, "Firmware: did not start");
    } else if (ota.firmware_present) {
        add_text(y, text_x, text_w, scale, kWhite, "Firmware: %s", ota.firmware_version);
    } else {
        add_text(y, text_x, text_w, scale, kRed, "Firmware: none");
    }
    y += 4;

    // The Wi-Fi: its state and, when connected, the address of the web page on the LAN
    switch (net.state) {
    case hc::network::StaState::Connected:
        add_text(y, text_x, text_w, scale, kGreen, "Wi-Fi: connected");
        add_text(y, text_x, text_w, scale, kWhite, "%s", net.ssid);
        add_text(y, text_x, text_w, scale, kWhite, "IP: %s", net.ip);
        break;
    case hc::network::StaState::NoNetworks:
        add_text(y, text_x, text_w, scale, kGrey, "Wi-Fi: not set up");
        break;
    default:
        add_text(y, text_x, text_w, scale, kWhite, "Wi-Fi: %s", hc::network::state_name(net.state));
        add_text(y, text_x, text_w, scale, kGrey, "%s", net.ssid);
        break;
    }
    if (net.error[0] != 0) {
        add_text(y, text_x, text_w, scale, kRed, "%s", net.error);
    }
    // The hotspot is always on in the recovery app and its QR code always joins it: the phone then
    // opens the captive portal, also when the Wi-Fi of the device is wrong or missing
    y += 4;
    add_text(y, text_x, text_w, scale, kWhite, "Hotspot:");
    add_text(y, text_x, text_w, scale, kWhite, "%s", net.hotspot_ssid);
    add_text(y, text_x, text_w, scale, kWhite, "Password:");
    add_text(y, text_x, text_w, scale, kWhite, "%s", net.hotspot_password);
    char qr_text[128];
    wifi_join_text(qr_text, sizeof(qr_text), net.hotspot_ssid, net.hotspot_password);

    // the QR code in the free space: a square card
    uint8_t temp[sizeof(g_qr)];
    if (!qrcodegen_encodeText(qr_text, temp, g_qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, kQrMaxVersion,
                              qrcodegen_Mask_AUTO, true)) {
        ESP_LOGW(kTag, "no QR code");
        return;
    }
    const int modules = qrcodegen_getSize(g_qr) + 2 * kQuietModules;
    const int line_h = 8 + 4;
    const int area_x = landscape ? w * 45 / 100 : 0;
    const int area_w = w - area_x;
    const int area_y = landscape ? kMargin : y;
    const int area_h = h - area_y - kMargin - 2 * line_h;
    const int side = std::min(area_w - 2 * kMargin, area_h);
    const int module_px = side / modules;
    if (module_px < 2) {
        return;
    }
    g_page.has_qr = true;
    g_page.module_px = module_px;
    const int card = modules * module_px;
    g_page.qr_x = area_x + (area_w - card) / 2;
    g_page.qr_y = area_y + (landscape ? (area_h - card) / 2 : 0);
    // under the code, centered: what it does and the address of the page on the hotspot
    const char *captions[2] = {"Scan to join the hotspot", "http://192.168.4.1/"};
    for (int i = 0; i < 2; i++) {
        char shown[48];
        std::snprintf(shown, sizeof(shown), "%s", captions[i]);
        fit(shown, static_cast<size_t>(std::max(0, (area_w - kMargin) / 8)));
        add_line(shown, area_x + (area_w - text_width(shown, 1)) / 2, g_page.qr_y + card + 4 + i * line_h, 1,
                 i == 0 ? kCyan : kWhite);
    }
}

// The page as one string: when it is the same as the drawn one nothing has changed
void signature(char *out, size_t size)
{
    size_t used = 0;
    out[0] = 0;
    for (int i = 0; i < g_page.line_count && used + 1 < size; i++) {
        used += static_cast<size_t>(std::snprintf(out + used, size - used, "%s|%d;", g_page.lines[i].text, g_page.lines[i].color));
    }
    if (g_page.has_qr && used + 1 < size) {
        std::snprintf(out + used, size - used, "qr%d,%d", g_page.qr_x, g_page.qr_y);
    }
}

void paint_text(const Line &line, int y0, int y1, uint16_t *band, int width, bool swap)
{
    const int size = 8 * line.scale;
    for (int y = std::max(y0, line.y); y < std::min(y1, line.y + size); y++) {
        const int row = (y - line.y) / line.scale;
        uint16_t *dst = band + static_cast<size_t>(y - y0) * width;
        const uint16_t color = swap ? __builtin_bswap16(line.color) : line.color;
        for (int i = 0; line.text[i] != 0; i++) {
            const unsigned char c = static_cast<unsigned char>(line.text[i]);
            const uint8_t bits = kFont[(c >= 32 && c < 127) ? c - 32 : '?' - 32][row];
            const int x0 = line.x + i * size;
            for (int col = 0; col < 8; col++) {
                if ((bits & (0x80 >> col)) == 0) {
                    continue;
                }
                for (int px = x0 + col * line.scale; px < x0 + (col + 1) * line.scale; px++) {
                    if (px >= 0 && px < width) {
                        dst[px] = color;
                    }
                }
            }
        }
    }
}

void paint_qr(int y0, int y1, uint16_t *band, int width, bool swap)
{
    const int n = qrcodegen_getSize(g_qr);
    const int modules = n + 2 * kQuietModules;
    const int card = modules * g_page.module_px;
    const uint16_t white = swap ? __builtin_bswap16(kWhite) : kWhite;
    const uint16_t black = swap ? __builtin_bswap16(kBlack) : kBlack;
    for (int y = std::max(y0, g_page.qr_y); y < std::min(y1, g_page.qr_y + card); y++) {
        const int my = (y - g_page.qr_y) / g_page.module_px - kQuietModules;
        uint16_t *dst = band + static_cast<size_t>(y - y0) * width + g_page.qr_x;
        for (int px = 0; px < card; px++) {
            const int mx = px / g_page.module_px - kQuietModules;
            const bool dark = mx >= 0 && mx < n && my >= 0 && my < n && qrcodegen_getModule(g_qr, mx, my);
            dst[px] = dark ? black : white;
        }
    }
}

void draw_page()
{
    const hc::board::Info &info = *g_screen.info;
    const int w = info.width;
    const int h = info.height;
    const int lines = info.draw_buffer_lines;
    const bool swap = info.swap_bytes;
    uint32_t counter = 0;
    for (int y = 0; y < h; y += lines) {
        const int y1 = std::min(y + lines, h);
        uint16_t *band = g_screen.bands[counter++ & 1];
        std::fill(band, band + static_cast<size_t>(w) * (y1 - y), swap ? __builtin_bswap16(kBlack) : kBlack);
        for (int i = 0; i < g_page.line_count; i++) {
            paint_text(g_page.lines[i], y, y1, band, w, swap);
        }
        if (g_page.has_qr) {
            paint_qr(y, y1, band, w, swap);
        }
        // y 0 starts the frame, every other band continues it (the panels that only take whole
        // frames need this order); the queued transfer of the previous band keeps its own buffer
        if (esp_lcd_panel_draw_bitmap(g_screen.handles.panel, 0, y, w, y1, band) != ESP_OK) {
            ESP_LOGE(kTag, "band at %d failed", y);
            return;
        }
    }
}

bool start_screen()
{
    g_screen.info = &hc::board::info();
    if (hc::board::init(g_screen.handles) != ESP_OK || g_screen.handles.panel == nullptr) {
        ESP_LOGE(kTag, "no screen");
        return false;
    }
    const size_t bytes = static_cast<size_t>(g_screen.info->width) * g_screen.info->draw_buffer_lines * 2;
    for (auto &band : g_screen.bands) {
        band = static_cast<uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if (band == nullptr) {
            ESP_LOGE(kTag, "no memory for the screen (%u bytes)", static_cast<unsigned>(bytes));
            return false;
        }
    }
    g_screen.ok = true;
    return true;
}

// Draws the page again when it changed; the backlight comes on after the first picture
void update_screen()
{
    static bool first = true;
    build_page();
    char signature_now[sizeof(g_drawn_signature)];
    signature(signature_now, sizeof(signature_now));
    if (!first && std::strcmp(signature_now, g_drawn_signature) == 0) {
        return;
    }
    std::strcpy(g_drawn_signature, signature_now);
    draw_page();
    if (first) {
        first = false;
        hc::board::set_backlight(80);
    }
}
} // namespace

extern "C" void app_main()
{
    hc::platform::log_system_info();
    const hc::ota::Info ota = hc::ota::info();
    ESP_LOGI(kTag, "recovery app on %s, firmware in flash: %s%s", hc::board::info().name,
             ota.firmware_present ? ota.firmware_version : "none",
             ota.firmware_failed ? " (did not start, rolled back)" : "");

    ESP_ERROR_CHECK(hc::platform::init_storage());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (hc::network::start(hc::platform::device_name(), nullptr, nullptr) != ESP_OK) {
        ESP_LOGE(kTag, "network did not start");
        return;
    }
    if (hc::webserver::start({.device_name = hc::platform::device_name()}) != ESP_OK) {
        ESP_LOGE(kTag, "web server did not start");
    }

    // the hotspot is always on here (not stored, the main firmware keeps its own setting)
    hc::network::force_hotspot(true);
    if (!start_screen()) {
        return;
    }
    for (;;) {
        update_screen();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
