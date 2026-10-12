# OV02C10 sensor driver

The camera module of the JC4880P443C carries an OmniVision OV02C10 (chip ID 0x5602 at SCCB
address 0x36); esp_cam_sensor has no driver for it. This one is copied unchanged from the vendor's
demo, `1-Demo/idf_examples/ESP-IDF/esp_brookesia_phone/components/espressif__esp_cam_sensor/
sensors/ov02c10` of https://github.com/wegi1/ESP32P4-JC4880P443C-I-W (Espressif's esp_cam_sensor
1.2.1 with the vendor's sensor added; the demo ran on the board as delivered).

License: Apache-2.0, Copyright 2024 Espressif Systems (Shanghai) CO LTD (file headers, `LICENSE`
of that component, copied here). Changes: line endings to LF; mirror and flip on bits 3 and 4 of 0x3820 with the window moved
by a pixel (the vendor's bits broke the readout); the gain table replaced by the analog gain alone in steps of 1/16 (the
vendor's did not match the sensor, the exposure jumped), `tline_ns` added to the formats' ISP info (esp_video 2.x computes
the exposure range from it, 0 failed the auto exposure); its Kconfig options are set in
components/camera/Kconfig instead of a menu; the IPA tuning `cfg/ov02c10_default.json` (the AE's frame delays 2 instead of 1, anti-flicker
for 60 Hz mains instead of 50: the exposure jumped between two values) is handed to
esp_ipa by components/camera/project_include.cmake.
