#include "hc/ui/bench.hpp"

#include <algorithm>
#include <cstdio>
#include <string_view>

#include "dashboard.hpp"
#include "hc/memory/large.hpp"
#include "overlay.hpp"
#include "rotation.hpp"
#include "settings.hpp"
#include "theme.hpp"

namespace hc::ui::bench {

namespace {

// how often the scenes look at the clock and move the finger
constexpr uint32_t kPollMs = 5;
// the time a page or a panel gets to settle before it is measured (the sample values arrive
// 150 ms after a page is subscribed, the history 300 ms, the camera image 500 ms)
constexpr uint32_t kSettleMs = 1500;
// redrawn frames of the scenes that redraw the whole screen
constexpr uint16_t kRedraws = 20;
// at most this many frames are kept per scene; a longer scene counts the rest only
constexpr size_t kMaxFrames = 600;

struct Frame {
    uint32_t total_us;  // the refresh, from the timer's start to its end
    uint32_t flush_us;  // of it: handing bands to the driver and waiting for them
    uint32_t pixels;
};

// A step of the run. A step without a name is not measured (it opens a page, waits for it). A
// measured step runs `ms`, or with `redraws` until that many frames were drawn, the whole screen
// invalidated again after each (at most `ms`).
struct Scene {
    const char *name;
    void (*begin)();
    uint32_t ms;
    uint16_t redraws = 0;
    // called every poll while the step runs, with the time since its start (a finger moving)
    void (*tick)(uint32_t elapsed_ms) = nullptr;
    // left out without Hooks::touch
    bool touches = false;
};

struct State {
    lv_disp_t *disp = nullptr;
    Hooks hooks{};
    lv_timer_t *poll = nullptr;
    lv_timer_t *refresh = nullptr;
    lv_timer_cb_t refresh_cb = nullptr;
    size_t scene = 0;
    bool begun = false;
    uint32_t started_ms = 0;
    uint64_t started_us = 0;
    // the flushes of the refresh that runs now
    uint32_t flush_us = 0;
    uint32_t flush_pixels = 0;
    uint32_t flushes = 0;
    // the frames of the scene that runs now, and how many there were in all
    memory::Vector<Frame> frames;
    uint32_t frame_count = 0;
    // a redraw scene: a frame was drawn since the last invalidation
    uint32_t invalidated_at_frame = 0;
    // the scroll scene's finger is on the screen
    bool finger_down = false;
    // the tiles and pages the scenes use, found at the start
    core::PageIndex dense = core::kNone;
    core::PageIndex large = core::kNone;
    core::TileIndex light = core::kNone;
    core::TileIndex climate = core::kNone;
    core::TileIndex sensor = core::kNone;
    core::TileIndex lock = core::kNone;
    core::TileIndex timer = core::kNone;
    core::TileIndex camera = core::kNone;
    core::TileIndex lamp = core::kNone;
    core::TileIndex weather = core::kNone;
    core::TileIndex automation = core::kNone;
    // the objects of the system layer (the performance monitor) hidden during the run
    memory::Vector<lv_obj_t *> hidden;
};

State g;

// ------------------------------------------------------------------------------- measuring

void on_flush(uint32_t us, uint32_t pixels)
{
    g.flush_us += us;
    g.flush_pixels += pixels;
    g.flushes++;
}

// The display driver's flush and wait, timed (`on`): on_flush() gets the time of every flush
// (handing a band to the driver) with its pixels, and of every wait for the band before it (0
// pixels). Off puts the driver's own callbacks back.
struct Driver {
    lv_disp_drv_t *driver = nullptr;
    void (*flush)(lv_disp_drv_t *, const lv_area_t *, lv_color_t *) = nullptr;
    void (*wait)(lv_disp_drv_t *) = nullptr;
};

Driver g_driver;

void measure_flushes(lv_disp_t *disp, bool on)
{
    if (!on) {
        if (g_driver.driver != nullptr) {
            g_driver.driver->flush_cb = g_driver.flush;
            g_driver.driver->wait_cb = g_driver.wait;
            g_driver = {};
        }
        return;
    }
    lv_disp_drv_t *driver = disp->driver;
    g_driver = {driver, driver->flush_cb, driver->wait_cb};
    driver->flush_cb = [](lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *pixels) {
        const uint64_t start = g.hooks.now_us();
        g_driver.flush(drv, area, pixels);
        on_flush(static_cast<uint32_t>(g.hooks.now_us() - start), lv_area_get_size(area));
    };
    // LVGL calls it in a loop until the band before is sent; it waits here, so one call is one wait
    driver->wait_cb = [](lv_disp_drv_t *drv) {
        const uint64_t start = g.hooks.now_us();
        while (drv->draw_buf->flushing) {
            if (g_driver.wait != nullptr) {
                g_driver.wait(drv);
            }
        }
        on_flush(static_cast<uint32_t>(g.hooks.now_us() - start), 0);
    };
}

// the display's refresh, timed: a refresh that flushed something is a frame
void on_refresh(lv_timer_t *timer)
{
    g.flush_us = 0;
    g.flush_pixels = 0;
    g.flushes = 0;
    const uint64_t start = g.hooks.now_us();
    g.refresh_cb(timer);
    const auto total = static_cast<uint32_t>(g.hooks.now_us() - start);
    if (g.flushes == 0 || g.flush_pixels == 0) {
        return;
    }
    g.frame_count++;
    if (g.frames.size() < kMaxFrames) {
        g.frames.push_back({total, std::min(g.flush_us, total), g.flush_pixels});
    }
}

struct Spread {
    float min_ms, median_ms, p95_ms, max_ms;
};

Spread spread(memory::Vector<uint32_t> &us)
{
    if (us.empty()) {
        return {};
    }
    std::sort(us.begin(), us.end());
    const auto at = [&us](size_t percent) { return static_cast<float>(us[(us.size() - 1) * percent / 100]) / 1000.0f; };
    return {at(0), at(50), at(95), at(100)};
}

void spread_json(char *out, size_t size, const char *key, const Spread &s)
{
    std::snprintf(out, size, "\"%s\":{\"min\":%.2f,\"med\":%.2f,\"p95\":%.2f,\"max\":%.2f}", key,
                  static_cast<double>(s.min_ms), static_cast<double>(s.median_ms), static_cast<double>(s.p95_ms),
                  static_cast<double>(s.max_ms));
}

void report(const char *name, uint32_t elapsed_ms)
{
    memory::Vector<uint32_t> total, render, flush;
    uint64_t pixels = 0;
    for (const Frame &f : g.frames) {
        total.push_back(f.total_us);
        render.push_back(f.total_us - f.flush_us);
        flush.push_back(f.flush_us);
        pixels += f.pixels;
    }
    char total_json[96], render_json[96], flush_json[96];
    spread_json(total_json, sizeof(total_json), "total", spread(total));
    spread_json(render_json, sizeof(render_json), "render", spread(render));
    spread_json(flush_json, sizeof(flush_json), "flush", spread(flush));
    const double fps = elapsed_ms > 0 ? g.frame_count * 1000.0 / elapsed_ms : 0.0;
    char line[512];
    std::snprintf(line, sizeof(line), "{\"scene\":\"%s\",\"frames\":%u,\"ms\":%u,\"fps\":%.1f,%s,%s,%s,\"pixels\":%u}",
                  name, static_cast<unsigned>(g.frame_count), static_cast<unsigned>(elapsed_ms), fps, total_json,
                  render_json, flush_json,
                  static_cast<unsigned>(g.frames.empty() ? 0 : pixels / g.frames.size()));
    g.hooks.line(line);
}

void memory_line(const char *when)
{
    if (g.hooks.memory == nullptr) {
        return;
    }
    char members[320] = "";
    g.hooks.memory(members, sizeof(members));
    char line[400];
    std::snprintf(line, sizeof(line), "{\"memory\":\"%s\",%s}", when, members);
    g.hooks.line(line);
}

// ------------------------------------------------------------------------------- the scenes

void invalidate_screen()
{
    lv_obj_invalidate(lv_scr_act());
    lv_obj_invalidate(lv_layer_top());
}

void open_main()
{
    dashboard::open_page(0);
}

void open_dense()
{
    dashboard::open_page(g.dense);
}

void open_large()
{
    dashboard::open_page(g.large);
}

void panel(core::TileIndex tile)
{
    if (tile != core::kNone) {
        dashboard::open_panel(tile);
    }
}

// the finger of the scroll: up the right margin of the screen in 300 ms (beside the rollers and the
// switches), then lifted, and the page throws on
void scroll_tick(uint32_t elapsed_ms)
{
    constexpr int32_t kMoveMs = 300;
    const int32_t w = lv_disp_get_hor_res(g.disp);
    const int32_t h = lv_disp_get_ver_res(g.disp);
    const auto x = static_cast<int16_t>(w - 5);
    const int32_t from = h * 4 / 5;
    const int32_t to = h / 5;
    const int32_t t = std::min(static_cast<int32_t>(elapsed_ms), kMoveMs);
    if (elapsed_ms < static_cast<uint32_t>(kMoveMs)) {
        g.finger_down = true;
        g.hooks.touch(x, static_cast<int16_t>(from + (to - from) * t / kMoveMs), true);
    } else if (g.finger_down) {
        g.finger_down = false;
        g.hooks.touch(x, static_cast<int16_t>(to), false);
    }
}

const Scene kScenes[] = {
    {nullptr, open_main, kSettleMs},
    {"main_redraw", nullptr, 3000, kRedraws},
    {"dense_slide_in", open_dense, 1000},
    {"dense_redraw", nullptr, 3000, kRedraws},
    {"dense_back", dashboard::back, 1000},
    {nullptr, nullptr, 500},
    {"light_panel_open", [] { panel(g.light); }, 1000},
    {"light_panel_redraw", nullptr, 3000, kRedraws},
    {"light_panel_close", dashboard::close_panel, 1000},
    {"climate_panel_open", [] { panel(g.climate); }, 1000},
    {"climate_panel_redraw", nullptr, 3000, kRedraws},
    {"climate_panel_close", dashboard::close_panel, 1000},
    {"sensor_panel_open", [] { panel(g.sensor); }, 1000},
    {"sensor_panel_redraw", nullptr, 3000, kRedraws},
    {"sensor_panel_close", dashboard::close_panel, 1000},
    {"lock_panel_open", [] { panel(g.lock); }, 1000},
    {"lock_panel_close", dashboard::close_panel, 1000},
    {"timer_panel_open", [] { panel(g.timer); }, 1000},
    {"timer_panel_close", dashboard::close_panel, 1000},
    {"picture_panel_open", [] { panel(g.camera); }, 1500},
    {"picture_panel_redraw", nullptr, 3000, kRedraws},
    {"picture_panel_close", dashboard::close_panel, 1000},
    {"pending_runner",
     [] {
         if (g.lamp != core::kNone) {
             core::Expectation off;
             off.state = "off";
             dashboard::expect(dashboard::layout().tiles[g.lamp].entity, off);
         }
     },
     2000},
    {nullptr, nullptr, 4000},
    {"alert_strip", [] { overlay::show(LV_SYMBOL_WARNING "  Benchmark", theme::kAlert, 1500); }, 2500},
    {"large_slide_in", open_large, 1000},
    {"clocks_tick", nullptr, 4000},
    {"weather_panel_open", [] { panel(g.weather); }, 1500},
    {"weather_panel_redraw", nullptr, 3000, kRedraws},
    {"weather_panel_close", dashboard::close_panel, 1000},
    {"automation_panel_open", [] { panel(g.automation); }, 1000},
    {"automation_panel_close", dashboard::close_panel, 1000},
    {nullptr, open_main, kSettleMs},
    {"settings_open", [] { settings::open(settings::Page::Alarm); }, 1500},
    {"settings_scroll", nullptr, 1500, 0, scroll_tick, true},
    {"settings_close", settings::close, 1000},
    {"rotate_90", rotation::rotate_clockwise, 2000},
    {"main_redraw_90", nullptr, 3000, kRedraws},
    {nullptr, rotation::rotate_clockwise, 1000},
    {nullptr, rotation::rotate_clockwise, 1000},
    {nullptr, rotation::rotate_clockwise, kSettleMs},
};

constexpr size_t kSceneCount = sizeof(kScenes) / sizeof(kScenes[0]);

// the tiles of the benchmark dashboard, by their entity's domain
void find_tiles()
{
    const core::Layout &layout = dashboard::layout();
    for (size_t i = 0; i < layout.tiles.size(); i++) {
        const core::Tile &tile = layout.tiles[i];
        const auto index = static_cast<core::TileIndex>(i);
        if (tile.opens != core::kNone) {
            (g.dense == core::kNone ? g.dense : g.large) = tile.opens;
            continue;
        }
        const std::string_view id = layout.entity_id(tile);
        const auto take = [&](std::string_view prefix, core::TileIndex &out) {
            if (out == core::kNone && id.starts_with(prefix)) {
                out = index;
            }
        };
        take("light.bench_", g.light);
        take("climate.bench_", g.climate);
        take("sensor.bench_", g.sensor);
        take("lock.bench_", g.lock);
        take("timer.bench_", g.timer);
        take("camera.bench_", g.camera);
        take("switch.bench_", g.lamp);
        take("weather.bench_", g.weather);
        take("automation.bench_", g.automation);
    }
}

void finish()
{
    lv_timer_del(g.poll);
    g.poll = nullptr;
    lv_timer_set_cb(g.refresh, g.refresh_cb);
    measure_flushes(g.disp, false);
    for (lv_obj_t *obj : g.hidden) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
    g.hidden.clear();
    memory_line("after");
    g.hooks.line("{\"end\":true}");
    g.frames = {};
    if (g.hooks.done != nullptr) {
        g.hooks.done();
    }
}

bool skipped(const Scene &scene)
{
    return scene.touches && g.hooks.touch == nullptr;
}

void on_poll(lv_timer_t *)
{
    const Scene &scene = kScenes[g.scene];
    if (!g.begun) {
        g.begun = true;
        g.started_ms = lv_tick_get();
        g.frames.clear();
        g.frame_count = 0;
        g.invalidated_at_frame = 0;
        // no dimming in the middle of a run
        lv_disp_trig_activity(g.disp);
        if (scene.begin != nullptr) {
            scene.begin();
        }
        if (scene.redraws > 0) {
            invalidate_screen();
        }
        return;
    }
    const uint32_t elapsed = lv_tick_elaps(g.started_ms);
    if (scene.tick != nullptr) {
        scene.tick(elapsed);
    }
    bool done = elapsed >= scene.ms;
    if (scene.redraws > 0 && !done) {
        if (g.frame_count >= scene.redraws) {
            done = true;
        } else if (g.frame_count > g.invalidated_at_frame) {
            g.invalidated_at_frame = g.frame_count;
            invalidate_screen();
        }
    }
    if (!done) {
        return;
    }
    if (scene.name != nullptr) {
        report(scene.name, elapsed);
    }
    do {
        g.scene++;
    } while (g.scene < kSceneCount && skipped(kScenes[g.scene]));
    g.begun = false;
    if (g.scene >= kSceneCount) {
        finish();
    }
}

} // namespace

bool start(lv_disp_t *disp, const Hooks &hooks, const char *header)
{
    if (g.poll != nullptr || !dashboard::shown()) {
        return false;
    }
    g = State{};
    g.disp = disp;
    g.hooks = hooks;
    find_tiles();
    g.frames.reserve(kMaxFrames);

    char line[400];
    std::snprintf(line, sizeof(line), "{\"bench\":%d,\"lvgl\":\"%d.%d.%d\",\"width\":%d,\"height\":%d%s%s}",
                  kScenesVersion, LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
                  static_cast<int>(lv_disp_get_hor_res(disp)), static_cast<int>(lv_disp_get_ver_res(disp)),
                  header != nullptr && header[0] != 0 ? "," : "", header != nullptr ? header : "");
    g.hooks.line(line);
    memory_line("before");

    // the performance monitor redraws its corner on its own: hidden, it draws nothing
    lv_obj_t *sys = lv_layer_sys();
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(sys); i++) {
        lv_obj_t *child = lv_obj_get_child(sys, static_cast<int32_t>(i));
        if (!lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
            g.hidden.push_back(child);
        }
    }
    settings::close();
    dashboard::close_panel();

    g.refresh = disp->refr_timer;
    g.refresh_cb = g.refresh->timer_cb;
    lv_timer_set_cb(g.refresh, on_refresh);
    measure_flushes(disp, true);
    g.poll = lv_timer_create(on_poll, kPollMs, nullptr);
    return true;
}

bool running()
{
    return g.poll != nullptr;
}

} // namespace hc::ui::bench
