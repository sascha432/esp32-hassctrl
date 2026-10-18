#pragma once

// The rendering benchmark (docs/benchmark.md): fixed scenes on the dashboard that is shown (the
// pages redrawn whole, a page sliding in, every panel opening and closing, a list scrolled, the
// clocks ticking, the alert strip, the pending runner, the turned screen), each timed per frame
// with the time of the refresh, the time spent flushing and the pixels. The results are lines of
// JSON. Made for the benchmark dashboard (bench/dashboard) on the sample backend; another dashboard
// gives numbers that cannot be compared. Plain LVGL: the firmware and the Windows version run it.

#include <cstddef>
#include <cstdint>

#include "lvgl.h"

namespace hc::ui::bench {

// raise it whenever a scene or what it measures changes (bench/VERSION is the dashboard's and the
// sample data's)
constexpr int kScenesVersion = 1;

struct Hooks {
    // microseconds since the start, steady
    uint64_t (*now_us)();
    // presses (true) or releases the pointer at a point of the screen as it is shown; nullptr: the
    // scenes that touch are left out
    void (*touch)(int16_t x, int16_t y, bool pressed);
    // memory figures as JSON members without braces ("\"internal_free\":1234,..."), called in the
    // LVGL task before and after the scenes; nullptr: none
    void (*memory)(char *out, size_t size);
    // one line of the result (JSON, no line end), called in the LVGL task
    void (*line)(const char *text);
    // the run ended, after the last line
    void (*done)();
};

// Starts the scenes on the dashboard. `header` (may be empty) holds JSON members without braces
// that go into the first line (board, firmware). Call with the LVGL lock held, after ui::start();
// false while a run goes on or no dashboard is shown.
bool start(lv_disp_t *disp, const Hooks &hooks, const char *header);

// a run goes on
bool running();

} // namespace hc::ui::bench
