#pragma once

// What the screens need from the rest of the firmware (docs/architecture.md section 3: the UI
// never talks to the network itself). `main` fills this table with the functions of the network,
// board and web server components. Every function is called from the LVGL task and returns at
// once; actions that take time (scans, connecting) report their progress through wifi_info().
// The one exception is save_dashboard, which writes a file of a few KB.

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string_view>

#include "hc/core/entity.hpp"
#include "hc/core/layout.hpp"

namespace hc::ui {

constexpr size_t kSsidSize = 33;
// WPA needs at least 8 characters; an open network has no password
constexpr size_t kMinPassword = 8;
constexpr size_t kMaxSavedNetworks = 5;

struct WifiInfo {
    bool connected;
    bool connecting;
    bool has_networks;
    // "connected", "scanning", ... in words
    char state[24];
    char ssid[kSsidSize];
    char ip[16];
    int8_t rssi;
    // the last problem in words, empty while there is none
    char error[64];
    bool hotspot_on;
    char hotspot_ssid[kSsidSize];
    char hotspot_password[12];
    uint8_t hotspot_clients;
    uint32_t hotspot_off_in_s;
    // the network has loaded its saved networks: has_networks is meaningful
    bool ready;
};

struct ScanItem {
    char ssid[kSsidSize];
    int8_t rssi;
    bool secure;
    bool saved;
};

struct DeviceInfo {
    char name[24];
    char board[24];
    char firmware[32];
    uint32_t uptime_s;
    uint32_t heap_internal_kb;
    uint32_t heap_psram_kb;
};

// The Home Assistant connection. Address and token are set in the web UI only.
struct HassInfo {
    bool configured; // address and token are stored
    bool connected;
    // the token was rejected, or the connection is down with a problem
    bool failed;
    char state[24]; // "connected", "token rejected", ... in words
    char url[128];
    char version[24];
    char error[96];
    uint32_t retry_in_s; // seconds until the next try while disconnected
};

// The screen's power settings (Settings, Display). The UI applies them; the backend stores them.
struct DisplaySettings {
    // percent, 1 to 100; with the automatic brightness the brightness in a bright room
    uint8_t brightness;
    uint8_t dim_brightness; // percent while dimmed, 1 to 100
    uint16_t dim_after_s;   // idle time before dimming, 0: never (up to 1 h)
    uint32_t off_after_s;   // idle time before the screen turns off, 0: never (up to 24 h)
    // follow the ambient light (only with a light sensor)
    bool auto_brightness;
    // display rotation in 90 degree steps (LVGL's), used when there is no orientation sensor or
    // it is locked
    uint8_t rotation;
    bool rotation_locked;
    // The automatic brightness runs linearly between two room lights (the sensor's lux, set by the
    // user in the room): at light_min_lux the screen is at its minimum, at light_max_lux and above
    // at `brightness`. light_min_lux < light_max_lux.
    float light_min_lux;
    float light_max_lux;
    // the motion sensor brightens a dimmed screen and turns it on, and keeps it on while it sees
    // motion (only with a motion sensor)
    bool wake_on_motion;
    // Home Assistant's motion helper stays on this long after the sensor's last motion,
    // 60 to 3600 s (only with a motion sensor; hc::motion::set_hold_s)
    uint32_t motion_hold_s;
};

// Settings, Sound. The UI plays the touch sounds; the backend stores the settings and sets the
// volume of the audio output. The touch sounds have switches of their own: they play whenever
// they are on and the volume is above 0. The alarm ignores all of it.
struct SoundSettings {
    uint8_t volume;    // percent, 0 (silent) to 100
    bool tap_sound;    // a click for every tap on something that reacts to it
    bool slider_sound; // ticks while a slider moves, more of them the faster it moves
};

// Settings, Sound, Microphone: a fixed gain, or the codec's automatic level control (then the
// gain does not count).
struct MicSettings {
    uint8_t gain_db; // 0 to kMicMaxGainDb
    bool auto_gain;
};

constexpr uint8_t kMicMaxGainDb = 60;

// the test recording of Settings, Sound: it records, then plays once
enum class MicRecording : uint8_t { Idle, Recording, Playing };

constexpr size_t kSoundNameSize = 32;

// Settings, Alarm (docs/ui/lvgl/lvgl-settings.md section 3): rings at full volume until Stop or Snooze.
struct AlarmSettings {
    bool on;
    uint8_t hour;
    uint8_t minute;
    // bit n: weekday n of std::tm (0 = Sunday); none: once, then the alarm turns itself off
    uint8_t days;
    uint8_t snooze_min;
    // a name of alarm_sounds(), empty: the built-in beeps
    char sound[kSoundNameSize];
    // the motor's pattern while it rings (with a motor); the Vibrate switch of Settings, Sound
    // does not count here
    bool vibrate;
};

constexpr size_t kMaxAlarms = 5;

// the alarms in the order they were added
struct Alarms {
    AlarmSettings list[kMaxAlarms];
    uint8_t count;
};

// Settings, Camera (docs/ui/lvgl/lvgl-settings.md section 3): the camera's picture, applied at once.
struct CameraPicture {
    bool auto_exposure;
    int8_t brightness;    // auto exposure: -50 to 50, moves the brightness it aims at
    uint16_t exposure_us; // manual
    uint8_t gain;         // manual: the analog gain in 1/16, 16 (1x) to 248
    bool mirror;
    bool flip;            // upside down
    uint8_t mains_hz;     // 50 or 60
};

constexpr size_t kCameraLoginSize = 33;

// Settings, Camera: the web stream (/api/camera.jpg and .mjpg); on only with a login.
struct CameraStream {
    bool enabled;
    bool half;            // half the width and height
    uint8_t max_fps;      // 5, 10, 15 or 30
    uint8_t quality;      // JPEG, 30 to 95
    char username[kCameraLoginSize];
    char password[kCameraLoginSize];
};

struct CameraInfo {
    uint16_t width; // the sensor's frame
    uint16_t height;
    uint16_t max_exposure_us;
};

// A camera's full screen view as it was left (docs/ui/lvgl/lvgl-camera.md): the zoom in percent
// and the pan as parts of the shown picture.
struct CameraView {
    float zoom;
    float pan_x;
    float pan_y;
};

// a finger on the screen, in the display's coordinates
struct Finger {
    int16_t x;
    int16_t y;
};

struct Backend {
    void (*wifi_info)(WifiInfo &out);
    // `password_needed` (may be nullptr): bit i set when network i was imported without its
    // password and is not tried until it gets one
    size_t (*saved_networks)(char (*ssids)[kSsidSize], size_t max, uint8_t *password_needed);
    size_t (*scan_results)(ScanItem *out, size_t max);
    void (*scan)();
    // saves the network and connects to it right away
    void (*add_network)(const char *ssid, const char *password);
    void (*remove_network)(uint8_t index);
    void (*move_network)(uint8_t index, int8_t delta);
    void (*reconnect)();
    void (*set_hotspot)(bool on);
    void (*new_hotspot_password)();

    void (*display_settings)(DisplaySettings &out);
    // keeps the settings and stores them a few seconds after the last change
    void (*set_display_settings)(const DisplaySettings &settings);
    // the backlight in percent of the perceived brightness, 0 turns it off
    void (*set_backlight)(uint8_t percent);
    // the light of the room in lux (calibrated, without the screen's own light); false without a
    // light sensor
    bool (*ambient_lux)(float &lux);
    // whether the motion sensor sees motion now; false without a motion sensor (board::Info::
    // motion_gpio), and then Settings have no "Wake on motion"
    bool (*motion)(bool &occupied);

    // Sound: nullptr when the firmware has no audio (CONFIG_HC_AUDIO); false when the audio output
    // did not start. Settings leave the Sound page out then.
    bool (*sound_settings)(SoundSettings &out);
    // applies the volume at once, stores the settings a few seconds after the last change
    void (*set_sound_settings)(const SoundSettings &settings);
    // play the touch sounds, whatever the settings say; nullptr when the firmware has none
    // (CONFIG_HC_AUDIO_TOUCH_SOUND)
    void (*play_touch_sound)();
    void (*play_slider_tick)();
    // Microphone: nullptr when the firmware has none (CONFIG_HC_MIC); false when it did not start.
    // Settings leave its section out then.
    bool (*mic_settings)(MicSettings &out);
    // applies at once, stores the settings a few seconds after the last change
    void (*set_mic_settings)(const MicSettings &settings);
    // The level meter: listen(true) while it shows, listen(false) after; the loudest sample since
    // the last call in dBFS, false while there is none.
    void (*mic_listen)(bool on);
    bool (*mic_level)(float &dbfs);
    // A few seconds recorded and played right after: start (false when it cannot: volume 0,
    // already busy), or stop and play what it has. Cancel ends it without playing (leaving the
    // page). The state, and the time left while it records.
    bool (*mic_record)(bool start);
    void (*mic_record_cancel)();
    MicRecording (*mic_recording)(uint32_t &remaining_ms);
    // Vibration: nullptr when the firmware has no motor (CONFIG_HC_VIBRATION); false when it did
    // not start. Settings show the switch on the Sound page then, and the page also without audio.
    // The switch is for taps; each alarm has its own (AlarmSettings::vibrate).
    bool (*vibration)(bool &on);
    // stores the switch a few seconds after the last change
    void (*set_vibration)(bool on);
    // the pulse for a tap, whatever the switch says
    void (*vibrate_click)();
    // The fingers on the screen now (0 to 2), for a gesture of two that LVGL's single pointer does
    // not see (the camera's pinch to zoom); nullptr without (the Windows version).
    uint8_t (*touch_fingers)(Finger out[2]);

    // The alarm: nullptr without it (CONFIG_HC_AUDIO_ALARM), and then Settings have no Alarm page.
    void (*alarms)(Alarms &out);
    // stores at once: an alarm has to survive a restart right after it was set
    void (*set_alarms)(const Alarms &alarms);
    // the names of the alarm sounds besides the built-in beeps (the MP3 files), sorted
    size_t (*alarm_sounds)(char (*names)[kSoundNameSize], size_t max);
    // `sound` at full volume ("" = the beeps); `loop` until stop_alarm(), else once (the preview);
    // the motor's pattern with it when `vibrate` (and a motor)
    void (*play_alarm)(const char *sound, bool loop, bool vibrate);
    void (*stop_alarm)();

    void (*hass_info)(HassInfo &out);
    // tries at once, also after a rejected token
    void (*hass_reconnect)();

    // The dashboard (docs/architecture.md section 4): the current layout, null without one. A
    // new pointer is a new dashboard (an import or an edit); the screen builds itself again.
    std::shared_ptr<const core::Layout> (*dashboard)();
    // Stores `layout` (the edit mode of the screen) as the dashboard if the stored one is still
    // `revision`; false with the problem in `error` (the web UI saved in between). dashboard()
    // returns the stored one afterwards.
    bool (*save_dashboard)(const core::Layout &layout, uint32_t revision, char *error, size_t error_size);
    // the values of the tiles of `page` (and the panel of the tile `panel`) are wanted from now
    // on, stamped with `generation`
    void (*subscribe_page)(std::shared_ptr<const core::Layout> layout, core::PageIndex page, core::Orientation o,
                           core::TileIndex panel,
                           uint32_t generation);
    void (*call_service)(const core::Intent &intent, std::string_view entity_id);
    // the next values or failure from Home Assistant, false when there is none
    bool (*take_event)(core::HassEvent &out);
    // The history of `entity_id` over the last `hours` for the graph of a sensor panel; it comes
    // back through take_event as a core::History stamped with `generation`.
    void (*fetch_history)(std::string_view entity_id, core::EntityIndex entity, core::HistoryKind kind, uint32_t hours,
                          uint32_t generation);
    // The forecast of the weather entity `entity_id` for the weather panel: it comes back through
    // take_event as a core::Forecast stamped with `generation`, and again with every update of the
    // integration, until the next call; `type` None ends it. nullptr: no forecasts.
    void (*subscribe_forecast)(std::string_view entity_id, core::EntityIndex entity, core::ForecastType type,
                               uint32_t generation);
    // The entity search of the edit mode (docs/ui/lvgl/lvgl-dashboard.md section 7): the entities
    // whose id or friendly name has every word of `query`, at most `max`, back through take_event
    // as a core::EntityList stamped with `generation`. nullptr: no adding of tiles on the screen.
    void (*find_entities)(std::string_view query, size_t max, uint32_t generation);
    // what a new tile needs of `entity_id`, back as a core::EntityDetails
    void (*fetch_entity)(std::string_view entity_id, uint32_t generation);
    // The camera image of `entity_id` as a `width` x `height` frame in the display's pixel format
    // (lv_color_t); it comes back through take_event as a core::Image stamped with `generation`.
    // `whole`: the whole picture instead, about as large as covers the box (the frame says its
    // size), not cut to it.
    void (*fetch_image)(std::string_view entity_id, core::EntityIndex entity, uint16_t width, uint16_t height, bool whole,
                        uint32_t generation);
    // Plays the MJPEG `stream` as frames of the whole picture, about as large as covers `width` x
    // `height` (core::Image with `live`, stamped with `generation`) until stop_stream() or the next
    // play_stream(); one plays at a time, the same one in another size goes on without a new
    // connection.
    void (*play_stream)(std::string_view entity_id, std::string_view stream, core::EntityIndex entity, uint16_t width,
                        uint16_t height, uint32_t generation);
    void (*stop_stream)();
    // The view of the camera `key` (its entity and stream) as it was left; false when none is
    // stored. set_camera_view stores it a few seconds later and keeps it across a restart.
    bool (*camera_view)(const char *key, CameraView &out);
    void (*set_camera_view)(const char *key, const CameraView &view);

    void (*device_info)(DeviceInfo &out);
    // "HH:MM", false while the clock is not set
    bool (*time_text)(char *out, size_t size);
    // the time in epoch seconds, 0 while the clock is not set
    int64_t (*clock)();
    // epoch seconds in the local time of the device (its time zone comes from Home Assistant)
    void (*local_time)(int64_t epoch, std::tm &out);
    // the address of the web UI ("http://192.168.0.206/"), empty without a network
    void (*web_address)(char *out, size_t size);

    void (*reset_web_login)();
    void (*restart)();
    void (*factory_reset)();

    // the one-time hint "Swipe down from the top for Settings" was used (stored)
    bool (*hint_seen)();
    void (*set_hint_seen)();

    // The camera: nullptr without one (CONFIG_HC_BOARD_HAS_CAMERA), and then Settings have no
    // Camera page; false when no sensor answered.
    bool (*camera_info)(CameraInfo &out);
    void (*camera_picture)(CameraPicture &out);
    // applies at once, stores a few seconds after the last change
    void (*set_camera_picture)(const CameraPicture &picture);
    void (*camera_stream)(CameraStream &out);
    // stores at once; the stream stays off without a username and a password
    void (*set_camera_stream)(const CameraStream &stream);
    // The preview: the camera's picture in the box x1, y1 to x2, y2 (inclusive, LVGL's
    // coordinates), drawn past LVGL so a moving picture does not tear; LVGL draws only outside
    // the box until camera_preview_stop(). A new start moves the box. nullptr without a preview
    // (the Windows version).
    void (*camera_preview_start)(int16_t x1, int16_t y1, int16_t x2, int16_t y2);
    void (*camera_preview_stop)();
};

} // namespace hc::ui
