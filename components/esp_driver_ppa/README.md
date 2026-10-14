# esp_driver_ppa override

Replaces ESP-IDF's `esp_driver_ppa` (a project component with an IDF component's name takes its
place). The CMake builds the IDF's own sources from `$IDF_PATH/components/esp_driver_ppa`, except
`ppa_srm.c`: the copy here, from ESP-IDF v6.1.0 (`fff9895c`), with one change.

## Why

On the JC4880P443C (ESP32-P4 rev v3.2) the PPA's first scale of a camera image after most boots
never finished (`hass/src/image_hw.cpp`; 4 of 4, 5 of 6 and 3 of 3 boots). Cause: silicon bug
DIG-734, the 2D-DMA miscounts a last macro block smaller than its FIFO and never raises the done
interrupt (https://github.com/espressif/esp-idf/issues/19023). The driver's workaround in
`ppa_srm_transaction_on_picked` sets the SRM engine's "bypass macro block order" flag only when its
formula predicts a leftover below the FIFO depth (1536 bits). Our scale (rotation 0, a 258x169
block of a 264x216 frame at 14/16 into a PSRAM buffer, RGB565, while the MIPI-DSI display scans
out of PSRAM) computes to 3696 bits, so the flag stayed clear and the scale hung anyway. The
issue's fix (469aa16, also for v5.5, v6.0 and v6.1) only corrects the formula for 90° and 270°
rotation and does not reach this case.

## The change

After the driver's formula, `bypass_mb_order = true;` always (a comment marks it). Measured on
2026-10-13 with the P4, cameras page opened right after the boot: with the flag 8 of 8 boots ran
(the scale in 2 to 8 ms), without it 3 of 3 hung. `image_hw.cpp` keeps its 500 ms timeout and
the software fallback.

A build directory that exists already does not notice a new component directory: run
`python tools/build.py <board> reconfigure` once (the first build of the P4 with this component
still linked the IDF's driver and hung as before).

## Keeping it with a new IDF

After an IDF update, compare the copy with the IDF's file:

```
diff $IDF_PATH/components/esp_driver_ppa/src/ppa_srm.c components/esp_driver_ppa/ppa_srm.c
```

Only the four added lines may differ. If more does, copy the IDF's file over this one and put the
line back. Drop the component once a release's formula covers this case (test: build without the
component, 8 boots with the cameras page, no `image_hw: the PPA did not scale` in the log).

License of `ppa_srm.c`: Apache-2.0, Copyright 2023-2026 Espressif Systems (Shanghai) CO LTD (file
header).
