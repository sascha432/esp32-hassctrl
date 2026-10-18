#pragma once

// Benchmark mode (docs/benchmark.md): the `bench start` console command restarts the panel into
// it; the boot after that runs the screens on the sample backend (components/sample_backend) with
// the benchmark dashboard built into the firmware (bench/dashboard), without network and Home
// Assistant, and `bench run` runs the scenes of hc/ui/bench.hpp. Nothing is stored: the next
// restart is a normal boot with the panel's own dashboard.

#include "hc/ui/backend.hpp"
#include "lvgl.h"

namespace bench_mode {

// true on the boot that `bench start` asked for (once: the next boot is a normal one again)
bool active();

// The sample backend with the benchmark dashboard and its fixed start time; the backlight is the
// board's. Only in benchmark mode.
const hc::ui::Backend &backend();

// the `bench` console command (CONFIG_HC_DEV_CONSOLE); `disp` is the display the scenes run on
// (nullptr: none, `bench run` says so)
void register_console(lv_disp_t *disp);

} // namespace bench_mode
