#pragma once

// A command line on the serial port for development (CONFIG_HC_DEV_CONSOLE). Type `help` for the
// commands.
//
// The console's task needs 10 KB of internal RAM in one piece for its stack (`screenshot` renders
// on it). Once Wi-Fi and Home Assistant are connected the internal RAM is too fragmented for that
// on the WT32-SC01, so the console is created early, in a task of its own that does not hold up
// app_main (the terminal probe and the drain of the boot log take seconds). The stored log level
// is applied then as well. Commands are taken once ready() says the components they use run.

#include "esp_err.h"

namespace hc::devconsole {

// Call right after the storage is initialized; returns at once.
esp_err_t start();

// The network and the other components run: the console takes commands from now on.
void ready();

} // namespace hc::devconsole
