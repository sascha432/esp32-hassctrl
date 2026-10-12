// Settings, Camera (docs/ui/screen.md section 5, Camera): only with a camera
// (Backend::camera_info). The page holds the mains frequency and the web stream; its login has a
// form of its own, and the preview shows the picture with the controls that change it.
//
// The preview's picture is not LVGL's: the backend draws it past LVGL into the box of the page
// (camera_preview_start), so a moving picture does not tear, and LVGL draws only around it. The
// box keeps the frame's shape: the whole picture, below the controls in portrait and beside them
// in landscape.

#include <cstdio>
#include <cstring>

#include "keyboard.hpp"
#include "settings.hpp"
#include "settings_pages.hpp"
#include "theme.hpp"

namespace hc::ui::settings::pages {

namespace {

namespace t = theme;

constexpr lv_coord_t kPreviewBarWidth = 300; // the controls beside the picture in landscape

CameraPicture picture()
{
    CameraPicture p{};
    backend().camera_picture(p);
    return p;
}

CameraStream stream()
{
    CameraStream s{};
    backend().camera_stream(s);
    return s;
}

CameraInfo info()
{
    CameraInfo i{};
    backend().camera_info(i);
    return i;
}

// ------------------------------------------------------------------------------- choice rows

// A row with a few buttons side by side, the chosen one lit. Deleted with its row.
struct Choice {
    void (*on_select)(int index);
    lv_obj_t *buttons[4];
    int count;
};

void choice_show(const Choice &c, int selected)
{
    for (int i = 0; i < c.count; i++) {
        lv_obj_set_style_bg_color(c.buttons[i], t::color(i == selected ? t::kActive : t::kBorder), 0);
    }
}

void choice_row(lv_obj_t *parent, const char *name, const char *const *labels, int count, int selected,
                void (*on_select)(int index))
{
    lv_obj_t *row = t::row(parent, name, "", false, nullptr);
    auto *c = new Choice{on_select, {}, count};
    lv_obj_add_event_cb(
        row, [](lv_event_t *e) { delete static_cast<Choice *>(lv_event_get_user_data(e)); }, LV_EVENT_DELETE, c);
    for (int i = 0; i < count; i++) {
        lv_obj_t *button = t::button(
            row, labels[i],
            [](lv_event_t *e) {
                auto &choice = *static_cast<Choice *>(lv_event_get_user_data(e));
                lv_obj_t *target = lv_event_get_target(e);
                for (int k = 0; k < choice.count; k++) {
                    if (choice.buttons[k] == target) {
                        choice_show(choice, k);
                        choice.on_select(k);
                    }
                }
            },
            c);
        lv_obj_set_height(button, 36);
        lv_obj_set_style_min_width(button, 52, 0);
        lv_obj_set_style_pad_hor(button, 10, 0);
        c->buttons[i] = button;
    }
    choice_show(*c, selected);
}

// --------------------------------------------------------------------------------- the page

lv_obj_t *g_stream_switch = nullptr;
lv_obj_t *g_login_row = nullptr;

void store_stream(const CameraStream &s)
{
    backend().set_camera_stream(s);
}

void login_text(char *out, size_t size)
{
    const CameraStream s = stream();
    std::snprintf(out, size, "%s", s.username[0] != 0 && s.password[0] != 0 ? s.username : "not set");
}

void quality_text(int32_t value, char *out, size_t size)
{
    std::snprintf(out, size, "%d", static_cast<int>(value));
}

constexpr uint8_t kFrameRates[] = {5, 10, 15, 30};

} // namespace

bool camera_available()
{
    CameraInfo i{};
    return backend().camera_info != nullptr && backend().camera_info(i);
}

Built camera_page(lv_obj_t *content)
{
    lv_obj_t *col = t::column(content);
    const CameraPicture p = picture();
    const CameraStream s = stream();
    const CameraInfo i = info();

    t::row(col, "Preview", "", true, [](lv_event_t *) { push(Page::CameraPreview); });

    section(col, "Picture");
    static const char *const kMains[] = {"50 Hz", "60 Hz"};
    choice_row(col, "Mains", kMains, 2, p.mains_hz == 50 ? 0 : 1, [](int index) {
        CameraPicture changed = picture();
        changed.mains_hz = index == 0 ? 50 : 60;
        backend().set_camera_picture(changed);
    });
    text_block(col, "The frequency of the power grid: the exposure keeps the lights from flickering in the "
                    "picture.");

    section(col, "Stream");
    lv_obj_t *stream_row = t::row(col, "Stream", "", false, nullptr);
    g_stream_switch = t::toggle(stream_row, s.enabled, [](lv_event_t *event) {
        CameraStream changed = stream();
        changed.enabled = lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED);
        store_stream(changed);
        if (changed.enabled && !stream().enabled) {
            // no login yet: the switch goes back, the login comes first
            lv_obj_clear_state(g_stream_switch, LV_STATE_CHECKED);
            push(Page::CameraLogin);
        }
    });
    char text[48];
    login_text(text, sizeof(text));
    g_login_row = t::row(col, "Login", text, true, [](lv_event_t *) { push(Page::CameraLogin); });

    char full[16];
    char half[16];
    std::snprintf(full, sizeof(full), "%ux%u", i.width, i.height);
    std::snprintf(half, sizeof(half), "%ux%u", i.width / 2, i.height / 2);
    static char s_full[16];
    static char s_half[16];
    std::memcpy(s_full, full, sizeof(full));
    std::memcpy(s_half, half, sizeof(half));
    static const char *const kSizes[] = {s_full, s_half};
    choice_row(col, "Size", kSizes, 2, s.half ? 1 : 0, [](int index) {
        CameraStream changed = stream();
        changed.half = index == 1;
        store_stream(changed);
    });
    static const char *const kRates[] = {"5", "10", "15", "30"};
    int rate = 3;
    for (int k = 0; k < 4; k++) {
        if (s.max_fps <= kFrameRates[k]) {
            rate = k;
            break;
        }
    }
    choice_row(col, "Frames/s", kRates, 4, rate, [](int index) {
        CameraStream changed = stream();
        changed.max_fps = kFrameRates[index];
        store_stream(changed);
    });
    slider_card(
        col, "Quality", 30, 95, s.quality,
        [](lv_event_t *event) {
            CameraStream changed = stream();
            changed.quality = static_cast<uint8_t>(lv_slider_get_value(lv_event_get_target(event)));
            store_stream(changed);
        },
        quality_text);

    char address[64] = {};
    if (backend().web_address != nullptr) {
        backend().web_address(address, sizeof(address));
    }
    char hint[320];
    if (address[0] != 0) {
        std::snprintf(hint, sizeof(hint),
                      "%sapi/camera.mjpg plays the stream, %sapi/camera.jpg is one picture. Both ask for the "
                      "login. A smaller size, fewer frames or a lower quality need less of the Wi-Fi.",
                      address, address);
    } else {
        std::snprintf(hint, sizeof(hint), "The stream is on the web server, once the device is on a network.");
    }
    text_block(col, hint);
    return {"Camera", nullptr};
}

// -------------------------------------------------------------------------------- the login

namespace {

lv_obj_t *g_user_area = nullptr;
lv_obj_t *g_secret_area = nullptr;
lv_obj_t *g_login_keyboard = nullptr;

void on_save(lv_event_t *)
{
    // OK or Enter in the name goes on to the password
    if (lv_keyboard_get_textarea(g_login_keyboard) == g_user_area) {
        focus_area(g_secret_area);
        return;
    }
    CameraStream changed = stream();
    const char *user = lv_textarea_get_text(g_user_area);
    const char *password = lv_textarea_get_text(g_secret_area);
    if (user[0] == 0) {
        // no name: no login, and the stream is off
        changed.username[0] = 0;
        changed.password[0] = 0;
        changed.enabled = false;
    } else {
        std::snprintf(changed.username, sizeof(changed.username), "%s", user);
        if (password[0] != 0) {
            std::snprintf(changed.password, sizeof(changed.password), "%s", password);
        } else if (changed.password[0] == 0) {
            focus_area(g_secret_area); // a new login needs a password
            return;
        }
    }
    store_stream(changed);
    set_form_dirty(false);
    pop();
}

} // namespace

Built camera_login_page(lv_obj_t *content)
{
    const CameraStream s = stream();
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *form = t::plain(content);
    lv_obj_set_width(form, lv_pct(100));
    lv_obj_set_flex_grow(form, 1);
    lv_obj_set_flex_flow(form, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(form, t::kPad, 0);
    lv_obj_set_style_pad_row(form, 6, 0);
    lv_obj_add_flag(form, LV_OBJ_FLAG_SCROLLABLE);

    auto line = [&]() {
        lv_obj_t *row = t::plain(form);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 6, 0);
        return row;
    };
    g_user_area = text_area(line(), "username", false);
    lv_textarea_set_max_length(g_user_area, kCameraLoginSize - 1);
    lv_textarea_set_text(g_user_area, s.username);
    lv_obj_t *second = line();
    // a stored password is not shown: empty keeps it
    g_secret_area = text_area(second, s.password[0] != 0 ? "password (unchanged)" : "password", true);
    lv_textarea_set_max_length(g_secret_area, kCameraLoginSize - 1);
    lv_obj_t *eye = t::button(second, LV_SYMBOL_EYE_OPEN, [](lv_event_t *) {
        lv_textarea_set_password_mode(g_secret_area, !lv_textarea_get_password_mode(g_secret_area));
    });
    lv_obj_set_style_min_width(eye, t::kRowHeight, 0);
    t::button(second, "Save", on_save);

    g_login_keyboard = keyboard::create(content);
    lv_obj_set_size(g_login_keyboard, lv_pct(100), lv_pct(55));
    // OK and Enter save, or go from the name to the password
    set_form(g_login_keyboard, g_user_area, g_secret_area, on_save);
    focus_area(s.username[0] != 0 ? g_secret_area : g_user_area);
    return {"Camera login", nullptr};
}

// ------------------------------------------------------------------------------ the preview

namespace {

lv_obj_t *g_box = nullptr;
lv_area_t g_box_area = {};

void exposure_text(int32_t value, char *out, size_t size)
{
    std::snprintf(out, size, "%d.%d ms", static_cast<int>(value / 10), static_cast<int>(value % 10));
}

void gain_text(int32_t value, char *out, size_t size)
{
    std::snprintf(out, size, "%d.%02dx", static_cast<int>(value / 16), static_cast<int>(value % 16 * 100 / 16));
}

void brightness_text(int32_t value, char *out, size_t size)
{
    std::snprintf(out, size, "%+d", static_cast<int>(value));
}

void store_picture(const CameraPicture &p)
{
    backend().set_camera_picture(p);
}

// starts the preview in the box, or moves it after a layout change (the display turned)
void place_preview()
{
    if (g_box == nullptr || backend().camera_preview_start == nullptr) {
        return;
    }
    lv_area_t area;
    lv_obj_get_coords(g_box, &area);
    if (std::memcmp(&area, &g_box_area, sizeof(area)) == 0) {
        return;
    }
    g_box_area = area;
    backend().camera_preview_start(static_cast<int16_t>(area.x1), static_cast<int16_t>(area.y1),
                                   static_cast<int16_t>(area.x2), static_cast<int16_t>(area.y2));
}

lv_obj_t *flag_button(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t callback)
{
    lv_obj_t *button = t::button(parent, text, callback);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_bg_color(button, t::color(t::kActive), LV_STATE_CHECKED);
    if (on) {
        lv_obj_add_state(button, LV_STATE_CHECKED);
    }
    lv_obj_set_flex_grow(button, 1);
    return button;
}

} // namespace

Built camera_preview_page(lv_obj_t *content)
{
    const CameraPicture p = picture();
    const CameraInfo i = info();
    lv_obj_update_layout(content);
    const lv_coord_t width = lv_obj_get_content_width(content);
    const lv_coord_t height = lv_obj_get_content_height(content);
    const bool landscape = width > height;
    lv_obj_set_flex_flow(content, landscape ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);

    // the box in the frame's shape: as wide as there is room, the controls take the rest
    lv_coord_t box_w = landscape ? width - kPreviewBarWidth : width;
    lv_coord_t box_h = i.width > 0 ? static_cast<lv_coord_t>(int32_t{box_w} * i.height / i.width) : box_w / 2;
    if (box_h > height) {
        box_h = height;
        box_w = i.height > 0 ? static_cast<lv_coord_t>(int32_t{box_h} * i.width / i.height) : box_w;
    }
    lv_obj_t *holder = t::plain(content);
    lv_obj_set_size(holder, landscape ? width - kPreviewBarWidth : width, landscape ? height : box_h);
    g_box = t::plain(holder);
    lv_obj_set_size(g_box, box_w, box_h);
    lv_obj_center(g_box);
    g_box_area = {};
    lv_obj_add_event_cb(
        g_box,
        [](lv_event_t *) {
            g_box = nullptr;
            if (backend().camera_preview_stop != nullptr) {
                backend().camera_preview_stop();
            }
        },
        LV_EVENT_DELETE, nullptr);

    lv_obj_t *bar = t::column(content);
    lv_obj_set_size(bar, landscape ? kPreviewBarWidth : width, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(bar, 1);

    lv_obj_t *auto_row = t::row(bar, "Auto exposure", "", false, nullptr);
    t::toggle(auto_row, p.auto_exposure, [](lv_event_t *event) {
        CameraPicture changed = picture();
        changed.auto_exposure = lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED);
        store_picture(changed);
        reload(); // the sliders of the other mode
    });
    if (p.auto_exposure) {
        slider_card(
            bar, "Brightness", -50, 50, p.brightness,
            [](lv_event_t *event) {
                CameraPicture changed = picture();
                changed.brightness = static_cast<int8_t>(lv_slider_get_value(lv_event_get_target(event)));
                store_picture(changed);
            },
            brightness_text);
    } else {
        // in 0.1 ms
        slider_card(
            bar, "Exposure", 1, i.max_exposure_us / 100, p.exposure_us / 100,
            [](lv_event_t *event) {
                CameraPicture changed = picture();
                changed.exposure_us = static_cast<uint16_t>(lv_slider_get_value(lv_event_get_target(event)) * 100);
                store_picture(changed);
            },
            exposure_text);
        slider_card(
            bar, "Gain", 16, 248, p.gain,
            [](lv_event_t *event) {
                CameraPicture changed = picture();
                changed.gain = static_cast<uint8_t>(lv_slider_get_value(lv_event_get_target(event)));
                store_picture(changed);
            },
            gain_text);
    }
    lv_obj_t *buttons = t::plain(bar);
    lv_obj_set_size(buttons, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, t::kPad, 0);
    flag_button(buttons, "Mirror", p.mirror, [](lv_event_t *event) {
        CameraPicture changed = picture();
        changed.mirror = lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED);
        store_picture(changed);
    });
    flag_button(buttons, "Flip", p.flip, [](lv_event_t *event) {
        CameraPicture changed = picture();
        changed.flip = lv_obj_has_state(lv_event_get_target(event), LV_STATE_CHECKED);
        store_picture(changed);
    });

    lv_obj_update_layout(content);
    place_preview();
    return {"Preview", place_preview};
}

} // namespace hc::ui::settings::pages
