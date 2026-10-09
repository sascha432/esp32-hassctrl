// The Windows version of the UI (docs/architecture.md section 1): the screens of components/ui on
// LVGL's Windows driver, with sample data from stub_backend.cpp. Only the look: no network, no
// Home Assistant. tools/sim.py builds and starts it; `hc_sim --help` lists the options.

#include <Windows.h>
#include <timeapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <memory>

#include "dashboard.hpp"
#include "dashboard_file.hpp"
#include "hc/core/layout.hpp"
#include "hc/ui/ui.hpp"
#include "lvgl.h"
#include "png.hpp"
#include "rotation.hpp"
#include "sample_hass.hpp"
#include "settings.hpp"
#include "stub_backend.hpp"
#include "win32drv/win32drv.h"

namespace {

namespace ui = hc::ui;

struct Options {
    int width = 480;
    int height = 320;
    int zoom = 1;
    int scenario = 0;
    uint8_t rotation = 0;
    bool sensor = false;
    // the look of the board the size belongs to
    ui::Options ui;
    bool lux_set = false;
    float lux = 0.0f;
    // docs/dashboard-sample/hass.yaml unless --dashboard names another
    std::string dashboard = HC_SOURCE_ROOT "/docs/dashboard-sample/hass.yaml";
    std::string page;
    std::string screenshot;
    uint32_t wait_ms = 1200;
    std::vector<std::string> steps;
    bool list = false;
};

struct Runtime {
    Options options;
    // no window on the screen and a clock that only moves when the script says so
    bool headless = false;
    bool quit = false;
    int exit_code = 0;
    int shot_number = 0;
    uint32_t title_tick = 0;
    char board[32] = "";
};

Runtime g;

constexpr uint32_t kStepMs = 10;

// ------------------------------------------------------------------------------- the clock

void update_title();
void handle_keys();

// Lets LVGL run for `ms`: timers, animations and the input devices. Headless this costs no real
// time; with a window it runs at the speed of the wall clock.
void pump(uint32_t ms)
{
    using Clock = std::chrono::steady_clock;
    static Clock::time_point last = Clock::now();
    uint32_t done = 0;
    while (done < ms && !g.quit) {
        if (g.headless) {
            lv_tick_inc(kStepMs);
            done += kStepMs;
        } else {
            Sleep(5);
            const Clock::time_point now = Clock::now();
            const uint32_t elapsed =
                static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count());
            // keeps the sub-millisecond rest for the next round
            last += std::chrono::milliseconds(elapsed);
            lv_tick_inc(elapsed);
            done += elapsed;
        }
        lv_timer_handler();
        handle_keys();
        if (lv_win32_quit_signal) {
            g.quit = true;
        }
    }
    update_title();
}

// ------------------------------------------------------------------------------- pages

struct Page {
    const char *name;
    const char *description;
    void (*open)();
};

void close_settings()
{
    ui::settings::close();
}

// Settings with the pages below each other, so `<` leads back like it does on the device.
void open_chain(std::initializer_list<ui::settings::Page> pages)
{
    auto it = pages.begin();
    ui::settings::open(*it);
    for (++it; it != pages.end(); ++it) {
        ui::settings::push(*it);
    }
}

const Page kPages[] = {
    {"dashboard", "the main page of the dashboard (the start screen with --dashboard none)",
     [] {
         close_settings();
         ui::dashboard::open_page(0);
     }},
    {"setup", "the first setup, welcome view", [] { ui::show_setup(); }},
    {"quick", "Settings, quick settings", [] { ui::settings::open(ui::settings::Page::Quick); }},
    {"wifi", "Settings, Wi-Fi", [] { ui::settings::open(ui::settings::Page::Wifi); }},
    {"network", "Settings, Wi-Fi, the first saved network",
     [] {
         char ssids[ui::kMaxSavedNetworks][ui::kSsidSize];
         const size_t count = sim::backend().saved_networks(ssids, ui::kMaxSavedNetworks);
         ui::settings::open(ui::settings::Page::Wifi);
         ui::settings::push(ui::settings::Page::NetworkDetail, count > 0 ? ssids[0] : "", 0);
     }},
    {"add-network", "Settings, Wi-Fi, Add network (the scan list)",
     [] { open_chain({ui::settings::Page::Wifi, ui::settings::Page::AddNetwork}); }},
    {"password", "Settings, Wi-Fi, Add network, password of a listed network",
     [] {
         ui::settings::open(ui::settings::Page::Wifi);
         ui::settings::push(ui::settings::Page::AddNetwork);
         ui::settings::push(ui::settings::Page::Password, "Guest");
     }},
    {"password-other", "the same for 'Other network ...' (name and password)",
     [] {
         ui::settings::open(ui::settings::Page::Wifi);
         ui::settings::push(ui::settings::Page::AddNetwork);
         ui::settings::push(ui::settings::Page::Password, "");
     }},
    {"hotspot", "Settings, Hotspot", [] { ui::settings::open(ui::settings::Page::Hotspot); }},
    {"home-assistant", "Settings, Home Assistant", [] { ui::settings::open(ui::settings::Page::HomeAssistant); }},
    {"display", "Settings, Display", [] { ui::settings::open(ui::settings::Page::Display); }},
    {"sound", "Settings, Sound", [] { ui::settings::open(ui::settings::Page::Sound); }},
    {"alarm", "Settings, Alarm", [] { ui::settings::open(ui::settings::Page::Alarm); }},
    {"ringing", "the alarm screen (the alarm rings now)", [] { ui::ring_alarm(); }},
    {"system", "Settings, System", [] { ui::settings::open(ui::settings::Page::System); }},
};

const Page *find_page(const char *name)
{
    for (const Page &page : kPages) {
        if (std::strcmp(page.name, name) == 0) {
            return &page;
        }
    }
    return nullptr;
}

// The page opens after the one before it is gone, like a tap would.
bool open_page(const char *name)
{
    const Page *page = find_page(name);
    if (page == nullptr) {
        std::fprintf(stderr, "unknown page '%s' (--list shows the pages)\n", name);
        return false;
    }
    if (ui::settings::is_open()) {
        close_settings();
        pump(300);
    }
    page->open();
    // the page is built in the next cycle, after its transition, and the first setup after its
    // timer: what a person sees a moment after the tap
    pump(800);
    return true;
}

// ------------------------------------------------------------------------------- input

// A label with this text on the screen, searching what lies on top first.
lv_obj_t *find_label(lv_obj_t *obj, const char *text, bool exact)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return nullptr;
    }
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *shown = lv_label_get_text(obj);
        if (exact ? std::strcmp(shown, text) == 0 : std::strstr(shown, text) != nullptr) {
            return obj;
        }
    }
    // the last child is the one drawn on top
    for (int32_t i = static_cast<int32_t>(lv_obj_get_child_cnt(obj)) - 1; i >= 0; i--) {
        if (lv_obj_t *found = find_label(lv_obj_get_child(obj, static_cast<uint32_t>(i)), text, exact)) {
            return found;
        }
    }
    return nullptr;
}

// The center of a label with this text, scrolled into view first like a finger would.
bool find_text(const char *text, lv_point_t &center)
{
    // a dialog on lv_layer_top covers the page: what lies on top wins, then the exact text
    for (lv_obj_t *root : {lv_layer_sys(), lv_layer_top(), lv_scr_act()}) {
        for (bool exact : {true, false}) {
            if (lv_obj_t *label = find_label(root, text, exact)) {
                lv_obj_scroll_to_view_recursive(label, LV_ANIM_OFF);
                lv_obj_update_layout(label);
                lv_area_t area;
                lv_obj_get_coords(label, &area);
                center = {static_cast<lv_coord_t>((area.x1 + area.x2) / 2),
                          static_cast<lv_coord_t>((area.y1 + area.y2) / 2)};
                return true;
            }
        }
    }
    return false;
}

void finger(lv_coord_t x, lv_coord_t y, bool pressed)
{
    lv_win32_pointer_override(true, x, y, pressed);
}

void tap(lv_coord_t x, lv_coord_t y)
{
    finger(x, y, false);
    pump(2 * LV_INDEV_DEF_READ_PERIOD);
    finger(x, y, true);
    pump(4 * LV_INDEV_DEF_READ_PERIOD);
    finger(x, y, false);
    // a page change takes a cycle to build and 200 ms to slide in; the next tap of a person comes
    // later than that
    pump(350);
    lv_win32_pointer_override(false, 0, 0, false);
}

void swipe(lv_coord_t x1, lv_coord_t y1, lv_coord_t x2, lv_coord_t y2, uint32_t ms)
{
    finger(x1, y1, false);
    pump(2 * LV_INDEV_DEF_READ_PERIOD);
    finger(x1, y1, true);
    pump(2 * LV_INDEV_DEF_READ_PERIOD);
    const uint32_t steps = std::max<uint32_t>(ms / LV_INDEV_DEF_READ_PERIOD, 1);
    for (uint32_t i = 1; i <= steps; i++) {
        finger(static_cast<lv_coord_t>(x1 + (x2 - x1) * static_cast<int>(i) / static_cast<int>(steps)),
               static_cast<lv_coord_t>(y1 + (y2 - y1) * static_cast<int>(i) / static_cast<int>(steps)), true);
        pump(LV_INDEV_DEF_READ_PERIOD);
    }
    finger(x2, y2, false);
    pump(2 * LV_INDEV_DEF_READ_PERIOD);
    lv_win32_pointer_override(false, 0, 0, false);
}

// ------------------------------------------------------------------------------- screenshot

bool screenshot(const std::string &path)
{
    lv_coord_t width = 0;
    lv_coord_t height = 0;
    const lv_color_t *frame = lv_win32_framebuffer(&width, &height);
    // what LVGL has not drawn yet (a change in the last tick) is drawn first
    lv_refr_now(nullptr);
    if (!sim::write_png(path.c_str(), frame, width, height)) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        g.exit_code = 3;
        return false;
    }
    std::printf("[sim] %s (%dx%d)\n", path.c_str(), width, height);
    std::fflush(stdout);
    return true;
}

// ------------------------------------------------------------------------------- keys and title

void select_scenario(int index)
{
    const int count = static_cast<int>(sim::scenario_count());
    sim::start_scenario(static_cast<size_t>((index % count + count) % count));
    // a boot with no network saved opens the first setup; the others start from the dashboard. (The
    // UI has no way to undo a first setup that is open: "Next" there leads on.)
    ui::settings::close();
    ui::WifiInfo wifi{};
    sim::backend().wifi_info(wifi);
    if (!wifi.has_networks) {
        ui::show_setup();
    }
    update_title();
}

void handle_keys()
{
    for (int key = lv_win32_pop_key(); key != 0; key = lv_win32_pop_key()) {
        const bool shift = (key & 0x100) != 0;
        key &= 0xff;
        if (key == 'R') {
            ui::rotation::rotate_clockwise();
        } else if (key == 'S') {
            select_scenario(static_cast<int>(sim::current_scenario()) + (shift ? -1 : 1));
        } else if (key >= '1' && key <= '9') {
            select_scenario(key - '1');
        } else if (key == VK_F12) {
            screenshot("hc_sim_" + std::to_string(++g.shot_number) + ".png");
        } else if (key == 'Q' || key == VK_ESCAPE) {
            g.quit = true;
        }
    }
}

void update_title()
{
    const uint32_t now = lv_tick_get();
    if (lv_tick_elaps(g.title_tick) < 250 && g.title_tick != 0) {
        return;
    }
    g.title_tick = now | 1;
    char title[256];
    std::snprintf(title, sizeof(title),
                  "hassctrl sim | %s %dx%d x%d | %s (%u of %u) | backlight %u %% | R rotate, S scenario, 1-9, F12 shot, Q quit",
                  g.board, static_cast<int>(lv_disp_get_hor_res(nullptr)), static_cast<int>(lv_disp_get_ver_res(nullptr)),
                  g.options.zoom, sim::scenario_info(sim::current_scenario()).name,
                  static_cast<unsigned>(sim::current_scenario() + 1), static_cast<unsigned>(sim::scenario_count()),
                  sim::backlight_percent());
    lv_win32_set_title(title);
}

// ------------------------------------------------------------------------------- script

std::vector<std::string> split(const std::string &text, char separator)
{
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); i++) {
        if (i == text.size() || text[i] == separator) {
            parts.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

bool numbers(const std::string &text, size_t count, int *out)
{
    const std::vector<std::string> parts = split(text, ',');
    if (parts.size() < count) {
        return false;
    }
    for (size_t i = 0; i < parts.size() && i < count + 1; i++) {
        char *end = nullptr;
        out[i] = static_cast<int>(std::strtol(parts[i].c_str(), &end, 10));
        if (end == parts[i].c_str() || *end != 0) {
            return false;
        }
    }
    return true;
}

bool run_step(const std::string &step)
{
    const size_t space = step.find(' ');
    const std::string verb = step.substr(0, space);
    const std::string argument = space == std::string::npos ? "" : step.substr(space + 1);
    int n[5] = {0, 0, 0, 0, 0};

    if (verb == "wait") {
        pump(static_cast<uint32_t>(std::atoi(argument.c_str())));
    } else if (verb == "page") {
        return open_page(argument.c_str());
    } else if (verb == "scenario") {
        const int index = sim::find_scenario(argument.c_str());
        if (index < 0) {
            std::fprintf(stderr, "unknown scenario '%s' (--list shows them)\n", argument.c_str());
            return false;
        }
        select_scenario(index);
        pump(800); // the first setup opens in the timer after it
    } else if (verb == "rotate") {
        ui::rotation::rotate_clockwise();
    } else if (verb == "shot") {
        pump(g.options.wait_ms);
        return screenshot(argument);
    } else if (verb == "tap") {
        lv_point_t at;
        if (numbers(argument, 2, n) && std::strchr(argument.c_str(), ',') != nullptr) {
            at = {static_cast<lv_coord_t>(n[0]), static_cast<lv_coord_t>(n[1])};
        } else if (!find_text(argument.c_str(), at)) {
            std::fprintf(stderr, "tap: no label '%s' on the screen\n", argument.c_str());
            return false;
        }
        tap(at.x, at.y);
    } else if (verb == "swipe") {
        if (!numbers(argument, 4, n)) {
            std::fprintf(stderr, "swipe needs x1,y1,x2,y2[,ms]\n");
            return false;
        }
        swipe(static_cast<lv_coord_t>(n[0]), static_cast<lv_coord_t>(n[1]), static_cast<lv_coord_t>(n[2]),
              static_cast<lv_coord_t>(n[3]), split(argument, ',').size() > 4 ? static_cast<uint32_t>(n[4]) : 300);
    } else {
        std::fprintf(stderr, "unknown step '%s' (--help lists them)\n", step.c_str());
        return false;
    }
    return true;
}

// ------------------------------------------------------------------------------- command line

void usage()
{
    std::puts(
        "hassctrl simulator: the screens on the PC, with sample data (docs/architecture.md section 1)\n"
        "\n"
        "  --size WxH|name      window size at rotation 0: 480x320 (default), 320x480, 320x240, 640x480,\n"
        "                       480x800; or wt32, jc3248, t4, vga, p4 (p4 and 480x800: pages\n"
        "                       slide, panels are dialogs, large fonts, like on the JC4880P443C)\n"
        "  --zoom N             every display pixel as N x N window pixels (default 1)\n"
        "  --dashboard FILE     the hass.yaml to show (default docs/dashboard-sample/hass.yaml); the\n"
        "                       .yaml files below its folder are its includes. `none`: no dashboard,\n"
        "                       the start screen stays\n"
        "  --scenario NAME      the sample state to start in (--list)\n"
        "  --page NAME          open this page after the start (--list)\n"
        "  --rotation 0-3       start in this rotation (LVGL's steps, counterclockwise)\n"
        "  --sensor             behave as a board with an orientation sensor (Rotate locks it)\n"
        "  --lux N              a light sensor with N lux in the room\n"
        "  --do STEP            a scripted step, repeatable, run in order after the start:\n"
        "                         page NAME | scenario NAME | rotate | wait MS | shot FILE\n"
        "                         tap TEXT (a label on the screen) | tap X,Y | swipe X1,Y1,X2,Y2[,MS]\n"
        "  --screenshot FILE    run the steps, wait, write the frame as a PNG and exit, without a window\n"
        "  --wait MS            settle time before a screenshot (default 1200)\n"
        "  --action-ms MS       an action changes the sample state this long after the tap (default 400)\n"
        "  --list               scenarios, pages and keys\n"
        "\n"
        "In the window: R rotate, S / Shift+S next / previous scenario, 1-9 a scenario, F12 screenshot, Q quit.\n"
        "The mouse is the finger. Without a window (--screenshot) time only moves when a step waits, so\n"
        "'--wait 11000' costs nothing.");
}

void list()
{
    std::puts("scenarios (--scenario, key S or 1-9):");
    for (size_t i = 0; i < sim::scenario_count(); i++) {
        std::printf("  %-18s %s\n", sim::scenario_info(i).name, sim::scenario_info(i).description);
    }
    std::puts("\npages (--page, step 'page'):");
    for (const Page &page : kPages) {
        std::printf("  %-18s %s\n", page.name, page.description);
    }
}

bool parse_size(const char *text, Options &o, char *board, size_t board_size)
{
    struct Preset {
        const char *name;
        int w, h;
        const char *board;
        // what board::Info says for the board (ui_slide_pages, ui_panel_dialogs)
        ui::Options ui;
    };
    static const Preset presets[] = {
        {"wt32", 480, 320, "WT32-SC01", {}},
        {"jc3248", 320, 480, "JC3248W535", {}},
        {"t4", 320, 240, "TTGO T4", {}},
        {"vga", 640, 480, "640x480 panel", {}},
        {"p4", 480, 800, "JC4880P443C", {.slide_pages = true, .panel_dialogs = true, .large_fonts = true, .tile_gap = 8}},
    };
    for (const Preset &p : presets) {
        if (std::strcmp(p.name, text) == 0) {
            o.width = p.w;
            o.height = p.h;
            o.ui = p.ui;
            std::snprintf(board, board_size, "%s", p.board);
            return true;
        }
    }
    int w = 0;
    int h = 0;
    if (std::sscanf(text, "%dx%d", &w, &h) != 2 || w < 100 || h < 100 || w > 2000 || h > 2000) {
        return false;
    }
    o.width = w;
    o.height = h;
    for (const Preset &p : presets) {
        if ((p.w == w && p.h == h) || (p.w == h && p.h == w)) {
            o.ui = p.ui;
            std::snprintf(board, board_size, "%s", p.board);
            return true;
        }
    }
    std::snprintf(board, board_size, "Sim %dx%d", w, h);
    return true;
}

bool parse(int argc, char **argv, Options &o)
{
    std::snprintf(g.board, sizeof(g.board), "WT32-SC01");
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        auto value = [&]() -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", arg.c_str());
                return nullptr;
            }
            return argv[++i];
        };
        const char *v = nullptr;
        if (arg == "--help" || arg == "-h") {
            usage();
            std::exit(0);
        } else if (arg == "--list") {
            o.list = true;
        } else if (arg == "--sensor") {
            o.sensor = true;
        } else if (arg == "--size") {
            if ((v = value()) == nullptr || !parse_size(v, o, g.board, sizeof(g.board))) {
                std::fprintf(stderr, "bad --size\n");
                return false;
            }
        } else if (arg == "--zoom") {
            if ((v = value()) == nullptr || (o.zoom = std::atoi(v)) < 1 || o.zoom > 8) {
                std::fprintf(stderr, "--zoom is 1 to 8\n");
                return false;
            }
        } else if (arg == "--scenario") {
            if ((v = value()) == nullptr || (o.scenario = sim::find_scenario(v)) < 0) {
                std::fprintf(stderr, "unknown scenario (--list)\n");
                return false;
            }
        } else if (arg == "--dashboard") {
            if ((v = value()) == nullptr) {
                return false;
            }
            o.dashboard = v;
        } else if (arg == "--page") {
            if ((v = value()) == nullptr) {
                return false;
            }
            o.page = v;
        } else if (arg == "--rotation") {
            if ((v = value()) == nullptr || std::atoi(v) < 0 || std::atoi(v) > 3) {
                std::fprintf(stderr, "--rotation is 0 to 3\n");
                return false;
            }
            o.rotation = static_cast<uint8_t>(std::atoi(v));
        } else if (arg == "--lux") {
            if ((v = value()) == nullptr) {
                return false;
            }
            o.lux_set = true;
            o.lux = static_cast<float>(std::atof(v));
        } else if (arg == "--do") {
            if ((v = value()) == nullptr) {
                return false;
            }
            o.steps.push_back(v);
        } else if (arg == "--screenshot") {
            if ((v = value()) == nullptr) {
                return false;
            }
            o.screenshot = v;
        } else if (arg == "--action-ms") {
            if ((v = value()) == nullptr) {
                return false;
            }
            sim::set_action_ms(static_cast<uint32_t>(std::atoi(v)));
        } else if (arg == "--wait") {
            if ((v = value()) == nullptr) {
                return false;
            }
            o.wait_ms = static_cast<uint32_t>(std::atoi(v));
        } else {
            std::fprintf(stderr, "unknown option %s (--help)\n", arg.c_str());
            return false;
        }
    }
    return true;
}

void log_to_console(const char *text)
{
    std::fputs(text, stderr);
    std::fflush(stderr);
}

} // namespace

int main(int argc, char **argv)
{
    Options &o = g.options;
    if (!parse(argc, argv, o)) {
        return 1;
    }
    if (o.list) {
        list();
        return 0;
    }
    g.headless = !o.screenshot.empty();

    // one display pixel is one window pixel, whatever the scaling of the desktop
    SetProcessDPIAware();
    // Sleep() in steps of 1 ms, not 15: smooth animations in the window
    timeBeginPeriod(1);

    lv_init();
    lv_log_register_print_cb(log_to_console);
    if (!lv_win32_init(GetModuleHandleW(nullptr), g.headless ? SW_HIDE : SW_SHOWNORMAL,
                       static_cast<lv_coord_t>(o.width), static_cast<lv_coord_t>(o.height), o.zoom)) {
        std::fprintf(stderr, "the window could not be created\n");
        return 1;
    }

    // read like the web UI's import; the dashboard screens take the layout from here
    if (o.dashboard != "none") {
        auto layout = std::make_shared<hc::core::Layout>();
        std::string error;
        if (!sim::load_dashboard(o.dashboard, *layout, error)) {
            std::fprintf(stderr, "%s: %s\n", o.dashboard.c_str(), error.c_str());
            return 2;
        }
        std::printf("[sim] dashboard %s: %u pages, %u tiles\n", o.dashboard.c_str(),
                    static_cast<unsigned>(layout->pages.size()), static_cast<unsigned>(layout->tiles.size()));
        sim::set_dashboard(std::move(layout));
    }

    sim::set_board(g.board);
    sim::set_rotation(o.rotation);
    if (o.lux_set) {
        sim::set_ambient_light(true, o.lux);
    }
    sim::start_scenario(static_cast<size_t>(o.scenario));

    ui::start(lv_disp_get_default(), sim::backend(), o.sensor, o.ui);
    // the boot: the first setup opens in the timer that follows
    pump(800);

    bool ok = true;
    if (!o.page.empty()) {
        ok = open_page(o.page.c_str());
    }
    for (size_t i = 0; ok && i < o.steps.size(); i++) {
        ok = run_step(o.steps[i]);
    }
    if (!ok) {
        return 2;
    }
    if (g.headless) {
        pump(o.wait_ms);
        return screenshot(o.screenshot) ? g.exit_code : 3;
    }
    while (!g.quit) {
        pump(50);
    }
    return g.exit_code;
}
