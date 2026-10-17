// The network task (see hc/network/network.hpp). Commands from other tasks and the events of the
// Wi-Fi driver arrive in one queue; only this task touches the driver and the state. Readers get
// copies of the status and the scan results under a short lock.

#include "hc/network/network.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"

#include "task.hpp"

namespace hc::network::detail {

namespace {

constexpr uint8_t kHotspotMaxClients = 4;

} // namespace

Task t;

QueueHandle_t g_queue = nullptr;
// set by the shutdown handler: the disconnect of a restart is not a lost connection
volatile bool g_restarting = false;
SemaphoreHandle_t g_lock = nullptr;

// copies for other tasks, guarded by g_lock
Status g_status{};
TickType_t g_setup_hotspot_until = 0;
SavedNetworkInfo g_saved[kMaxNetworks] = {};
size_t g_saved_count = 0;
ScanEntry g_scan[kMaxScan] = {};
size_t g_scan_count = 0;

namespace {

Listener g_listener = nullptr;
void *g_listener_context = nullptr;

} // namespace

TickType_t now()
{
    return xTaskGetTickCount();
}

bool reached(TickType_t deadline)
{
    return static_cast<int32_t>(now() - deadline) >= 0;
}

// copies at most size - 1 characters and always terminates (SSIDs from the driver are not
// terminated when they use all 32 bytes)
void copy_string(char *out, size_t size, const char *in)
{
    const size_t length = strnlen(in, size - 1);
    std::memcpy(out, in, length);
    out[length] = 0;
}

namespace {

void publish()
{
    t.status.saved_count = t.stored.count;
    t.status.policy = t.stored.policy;
    t.status.hotspot_enabled = t.stored.hotspot_enabled;
    copy_string(t.status.hotspot_password, sizeof(t.status.hotspot_password), t.stored.hotspot_password);

    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_status = t.status;
    g_setup_hotspot_until = t.setup_hotspot_until;
    g_saved_count = t.stored.count;
    for (size_t i = 0; i < t.stored.count; i++) {
        const SavedNetwork &network = t.stored.networks[i];
        copy_string(g_saved[i].ssid, kSsidSize, network.ssid);
        g_saved[i].open = network.password[0] == 0 && !network.password_needed;
        g_saved[i].password_needed = network.password_needed;
    }
    xSemaphoreGive(g_lock);

    if (g_listener != nullptr) {
        g_listener(t.status, g_listener_context);
    }
}

} // namespace

void set_error(const char *text)
{
    copy_string(t.status.error, sizeof(t.status.error), text);
}

// ----------------------------------------------------------------------------------- hotspot

namespace {

esp_err_t configure_hotspot()
{
    wifi_config_t config{};
    copy_field(config.ap.ssid, t.device_name, kSsidSize);
    config.ap.ssid_len = static_cast<uint8_t>(strnlen(t.device_name, sizeof(config.ap.ssid)));
    copy_field(config.ap.password, t.stored.hotspot_password, sizeof(t.stored.hotspot_password));
    config.ap.max_connection = kHotspotMaxClients;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    // the station decides the channel once it is connected
    config.ap.channel = 1;
    return esp_wifi_set_config(WIFI_IF_AP, &config);
}

} // namespace

// saved networks that may be tried (not waiting for their password)
size_t usable_count()
{
    size_t count = 0;
    for (size_t i = 0; i < t.stored.count; i++) {
        count += t.stored.networks[i].password_needed ? 0 : 1;
    }
    return count;
}

namespace {

// without a network to try, the hotspot is the way to set one up, as at the first setup
bool hotspot_wanted()
{
    return usable_count() == 0 || t.stored.hotspot_enabled || t.setup_hotspot_until != 0 ||
           t.setup_failed || t.force_hotspot;
}

void update_hotspot()
{
    const bool wanted = hotspot_wanted();
    if (wanted == t.status.hotspot_on) {
        return;
    }
    if (wanted) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        configure_hotspot();
    } else {
        esp_wifi_set_mode(WIFI_MODE_STA);
        t.status.hotspot_clients = 0;
    }
    t.status.hotspot_on = wanted;
    ESP_LOGI(kTag, "hotspot %s", wanted ? "on" : "off");
}

// ------------------------------------------------------------------------------ the task

void handle_wifi_event(int32_t event, uint8_t reason)
{
    switch (event) {
    case WIFI_EVENT_STA_START:
        start_connect();
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        on_disconnected(reason);
        break;
    case WIFI_EVENT_SCAN_DONE:
        on_scan_done();
        break;
    case WIFI_EVENT_AP_STACONNECTED:
        t.status.hotspot_clients++;
        break;
    case WIFI_EVENT_AP_STADISCONNECTED:
        if (t.status.hotspot_clients > 0) {
            t.status.hotspot_clients--;
        }
        break;
    default:
        return;
    }
}

void handle(const Msg &msg)
{
    switch (msg.type) {
    case MsgType::Add:
        cmd_add(msg);
        break;
    case MsgType::Remove:
        cmd_remove(msg.index);
        break;
    case MsgType::Move:
        cmd_move(msg.index, msg.value);
        break;
    case MsgType::SetPolicy:
        t.stored.policy = static_cast<Policy>(msg.value);
        save_policy(t.stored);
        break;
    case MsgType::Reconnect:
        cmd_reconnect();
        break;
    case MsgType::Scan:
        start_scan(ScanPurpose::User);
        break;
    case MsgType::Hotspot:
        t.stored.hotspot_enabled = msg.value != 0;
        if (!t.stored.hotspot_enabled) {
            // off means off, also during the time after the setup
            t.setup_hotspot_until = 0;
        }
        save_hotspot(t.stored);
        break;
    case MsgType::ForceHotspot:
        t.force_hotspot = msg.value != 0;
        break;
    case MsgType::NewHotspotPassword:
        generate_hotspot_password(t.stored.hotspot_password);
        save_hotspot(t.stored);
        if (t.status.hotspot_on) {
            configure_hotspot();
        }
        ESP_LOGI(kTag, "new hotspot password");
        break;
    case MsgType::Import: {
        const ImportJob &job = *static_cast<const ImportJob *>(msg.data);
        cmd_import(job);
        xSemaphoreGive(job.done);
        break;
    }
    case MsgType::WifiEvent:
        handle_wifi_event(msg.event, msg.reason);
        break;
    case MsgType::GotIp:
        on_got_ip(msg.ip);
        break;
    }
}

void handle_timers()
{
    if (t.status.state == StaState::WaitingToRetry && reached(t.retry_at)) {
        start_connect();
    }
    if (t.status.state == StaState::Connected && reached(t.rssi_check_at)) {
        check_signal();
    }
    if (t.setup_hotspot_until != 0 && reached(t.setup_hotspot_until)) {
        t.setup_hotspot_until = 0;
        ESP_LOGI(kTag, "setup hotspot time is over");
    }
    update_hotspot();
}

// the time until the next timer, at most a second (keeps the countdown in the status current)
TickType_t next_wait()
{
    TickType_t wait = pdMS_TO_TICKS(1000);
    auto consider = [&wait](TickType_t deadline) {
        const int32_t left = static_cast<int32_t>(deadline - now());
        wait = std::min<TickType_t>(wait, left > 0 ? static_cast<TickType_t>(left) : 0);
    };
    if (t.status.state == StaState::WaitingToRetry) {
        consider(t.retry_at);
    }
    if (t.status.state == StaState::Connected) {
        consider(t.rssi_check_at);
    }
    return wait;
}

void task(void *)
{
    Msg msg;
    for (;;) {
        if (xQueueReceive(g_queue, &msg, next_wait()) == pdTRUE) {
            handle(msg);
        }
        handle_timers();
        publish();
    }
}

// ------------------------------------------------------------------------- driver events

bool post(const Msg &msg)
{
    return g_queue != nullptr && xQueueSend(g_queue, &msg, 0) == pdTRUE;
}

void on_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    Msg msg{};
    if (base == WIFI_EVENT) {
        msg.type = MsgType::WifiEvent;
        msg.event = id;
        if (id == WIFI_EVENT_STA_DISCONNECTED) {
            msg.reason = static_cast<uint8_t>(static_cast<wifi_event_sta_disconnected_t *>(data)->reason);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        msg.type = MsgType::GotIp;
        msg.ip = static_cast<ip_event_got_ip_t *>(data)->ip_info.ip.addr;
    } else {
        return;
    }
    if (!post(msg)) {
        ESP_LOGW(kTag, "event %ld dropped, queue full", static_cast<long>(id));
    }
}

// The device answers as <device name>.local on the Wi-Fi and the hotspot, and announces its web
// server, so the address on the screen works without knowing the IP. Without it the device is
// still reachable by its address.
void start_mdns(const char *device_name)
{
    esp_err_t err = mdns_init();
    if (err == ESP_OK) {
        err = mdns_hostname_set(device_name);
    }
    if (err == ESP_OK) {
        err = mdns_instance_name_set(device_name);
    }
    if (err == ESP_OK) {
        err = mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
    }
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "mDNS did not start (%s), the device is reachable by its address only", esp_err_to_name(err));
    }
}

} // namespace

} // namespace hc::network::detail

namespace hc::network {

using namespace detail;

esp_err_t start(const char *device_name, Listener listener, void *context)
{
    ESP_RETURN_ON_FALSE(g_queue == nullptr, ESP_ERR_INVALID_STATE, kTag, "already started");
    g_lock = xSemaphoreCreateMutex();
    g_queue = xQueueCreate(16, sizeof(Msg));
    ESP_RETURN_ON_FALSE(g_lock != nullptr && g_queue != nullptr, ESP_ERR_NO_MEM, kTag, "queue");
    g_listener = listener;
    g_listener_context = context;

    t.device_name = device_name;
    ESP_RETURN_ON_ERROR(load(t.stored), kTag, "settings");
    copy_string(t.status.hotspot_ssid, sizeof(t.status.hotspot_ssid), device_name);
    t.status.state = StaState::NoNetworks;
    // still the first setup: nothing saved, or no saved network ever connected (no access point
    // remembered), so a restart with a wrong password gets the hotspot back as well
    t.setup_hotspot = usable_count() == 0 || t.stored.last_index == kNoIndex;
    t.fast_allowed = esp_reset_reason() == ESP_RST_DEEPSLEEP;

    ESP_RETURN_ON_ERROR(esp_netif_init(), kTag, "netif");
    t.sta = esp_netif_create_default_wifi_sta();
    t.ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(t.sta, device_name);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), kTag, "wifi init");
    // Shutdown handlers run in reverse order: this one runs before the Wi-Fi driver stops.
    // Leaving the network on purpose sends the deauthentication the router waits for; without it
    // the router still holds the old connection after the restart and turns the first attempt away.
    esp_register_shutdown_handler([] {
        g_restarting = true;
        if (esp_wifi_disconnect() == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    });
    // the network component is the only owner of the stored networks (docs/architecture.md 9.3)
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), kTag, "storage");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr), kTag, "events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr), kTag, "events");
    start_mdns(device_name);

    t.status.hotspot_on = hotspot_wanted();
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(t.status.hotspot_on ? WIFI_MODE_APSTA : WIFI_MODE_STA), kTag, "mode");
    if (t.status.hotspot_on) {
        ESP_RETURN_ON_ERROR(configure_hotspot(), kTag, "hotspot");
    }

    t.status.ready = true;
    publish();
    ESP_RETURN_ON_FALSE(xTaskCreate(task, "network", 4096, nullptr, 5, nullptr) == pdPASS, ESP_ERR_NO_MEM, kTag,
                        "task");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "wifi start");
    // No modem sleep: with it every packet waits for the next beacon (ping 18 to 200 ms, single
    // web requests up to seconds on the T4). The panels run on mains power; a battery remote
    // will need its own choice here.
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(kTag, "%u saved networks, hotspot '%s' %s", t.stored.count, device_name,
             t.status.hotspot_on ? "on" : "off");
    return ESP_OK;
}

} // namespace hc::network
