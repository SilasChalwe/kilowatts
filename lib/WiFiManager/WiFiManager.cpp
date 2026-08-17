/**
 * @file WiFiManager.cpp
 * @brief Implements Central Node infrastructure Wi-Fi station connectivity.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
 */

#include "WiFiManager.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

namespace kilowatts {


static const char *TAG = "WIFI_MANAGER";


namespace {

/* Exponential backoff bounds for reconnection after a disconnect event. */
constexpr std::uint64_t RECONNECT_BASE_DELAY_MICROSECONDS = 2000000ULL;   // 2 s
constexpr std::uint64_t RECONNECT_MAXIMUM_DELAY_MICROSECONDS = 60000000ULL; // 60 s
constexpr std::uint32_t RECONNECT_BACKOFF_CAP_SHIFT = 5U; // 2^5 * base = 64s, already clamped below

esp_timer_handle_t reconnectTimer = nullptr;

} // namespace


WiFiManager* WiFiManager::instance_ = nullptr;


WiFiManager::WiFiManager(std::uint8_t requiredChannel)
    : requiredChannel_(requiredChannel),
      credentials_{"", "", nullptr},
      state_(WiFiConnectionState::DISCONNECTED),
      reconnectAttempts_(0U),
      eventHandlersRegistered_(false)
{
    instance_ = this;
}


WiFiManager::~WiFiManager()
{
    if (reconnectTimer != nullptr) {
        esp_timer_stop(reconnectTimer);
        esp_timer_delete(reconnectTimer);
        reconnectTimer = nullptr;
    }

    if (eventHandlersRegistered_) {
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &WiFiManager::handleWiFiEvent);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &WiFiManager::handleIpEvent);
    }

    if (instance_ == this) {
        instance_ = nullptr;
    }
}


bool WiFiManager::begin(const Credentials& credentials)
{
    if (credentials.ssid == nullptr || credentials.ssid[0] == '\0') {
        ESP_LOGE(TAG, "begin() rejected: no SSID configured (see include/KilowattsSecrets.h)");
        return false;
    }

    credentials_ = credentials;

    if (credentials.hostname != nullptr && credentials.hostname[0] != '\0') {
        esp_netif_t* staNetif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (staNetif != nullptr) {
            esp_netif_set_hostname(staNetif, credentials.hostname);
        } else {
            ESP_LOGW(TAG, "Could not find the Wi-Fi station netif to set its DHCP hostname");
        }
    }

    esp_err_t result = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &WiFiManager::handleWiFiEvent, this, nullptr);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not register Wi-Fi event handler: %s", esp_err_to_name(result));
        return false;
    }

    result = esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &WiFiManager::handleIpEvent, this, nullptr);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not register IP event handler: %s", esp_err_to_name(result));
        return false;
    }

    eventHandlersRegistered_ = true;

    const esp_timer_create_args_t timerArgs = {
        [](void* arg) {
            WiFiManager* self = static_cast<WiFiManager*>(arg);
            if (self != nullptr) {
                self->verifyChannelAndConnect();
            }
        },
        this,
        ESP_TIMER_TASK,
        "kw_wifi_reconnect",
        true
    };
    esp_timer_create(&timerArgs, &reconnectTimer);

    return verifyChannelAndConnect();
}


bool WiFiManager::verifyChannelAndConnect()
{
    state_.store(WiFiConnectionState::SCANNING);

    wifi_scan_config_t scanConfig{};
    scanConfig.ssid = reinterpret_cast<std::uint8_t*>(const_cast<char*>(credentials_.ssid));
    scanConfig.show_hidden = true;

    /*
     * A blocking scan (block = true) is used here deliberately: the
     * caller (Central's WiFiManager initialisation, or the reconnect
     * timer callback) is not on a latency-sensitive path, and a blocking
     * scan lets verifyChannelAndConnect() make its channel decision and
     * either call esp_wifi_connect() or refuse in one straight-line
     * function without an extra asynchronous scan-done handoff.
     */
    esp_err_t result = esp_wifi_scan_start(&scanConfig, true);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan for SSID '%s' failed to start: %s", credentials_.ssid, esp_err_to_name(result));
        onStationDisconnected(nullptr);
        return false;
    }

    std::uint16_t apCount = 0U;
    esp_wifi_scan_get_ap_num(&apCount);

    if (apCount == 0U) {
        ESP_LOGW(TAG, "SSID '%s' was not found in the scan; retrying with backoff", credentials_.ssid);
        onStationDisconnected(nullptr);
        return false;
    }

    std::vector<wifi_ap_record_t> records(apCount);
    esp_wifi_scan_get_ap_records(&apCount, records.data());

    bool found = false;
    std::uint8_t discoveredChannel = 0U;

    for (std::uint16_t i = 0U; i < apCount; ++i) {
        if (std::strncmp(reinterpret_cast<const char*>(records[i].ssid), credentials_.ssid, sizeof(records[i].ssid)) == 0) {
            found = true;
            discoveredChannel = records[i].primary;
            break;
        }
    }

    if (!found) {
        ESP_LOGW(TAG, "SSID '%s' not present among %u scanned networks; retrying with backoff",
                 credentials_.ssid, static_cast<unsigned int>(apCount));
        onStationDisconnected(nullptr);
        return false;
    }

    if (discoveredChannel != requiredChannel_) {
        /*
         * Refuse to connect rather than letting esp_wifi_connect() force
         * the shared radio onto a different channel and silently break
         * ESP-NOW to every Smart Node. Local ESP-NOW control is
         * unaffected; only MQTT/Wi-Fi connectivity stays unavailable
         * until the Access Point is reconfigured onto
         * KILOWATTS_RADIO_CHANNEL.
         */
        ESP_LOGE(TAG,
                 "RADIO_CHANNEL_MISMATCH: SSID '%s' is broadcasting on channel %u but ESP-NOW requires channel %u. "
                 "Reconfigure the Access Point onto the required channel — local ESP-NOW control is unaffected, "
                 "but MQTT/Wi-Fi connectivity stays unavailable until this is corrected.",
                 credentials_.ssid, static_cast<unsigned int>(discoveredChannel), static_cast<unsigned int>(requiredChannel_));
        state_.store(WiFiConnectionState::RADIO_CHANNEL_MISMATCH);
        return false;
    }

    wifi_config_t wifiConfig{};
    std::snprintf(reinterpret_cast<char*>(wifiConfig.sta.ssid), sizeof(wifiConfig.sta.ssid), "%s", credentials_.ssid);
    std::snprintf(reinterpret_cast<char*>(wifiConfig.sta.password), sizeof(wifiConfig.sta.password), "%s", credentials_.password);
    wifiConfig.sta.channel = requiredChannel_;

    result = esp_wifi_set_config(WIFI_IF_STA, &wifiConfig);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config() failed: %s", esp_err_to_name(result));
        onStationDisconnected(nullptr);
        return false;
    }

    state_.store(WiFiConnectionState::CONNECTING);
    result = esp_wifi_connect();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect() failed: %s", esp_err_to_name(result));
        onStationDisconnected(nullptr);
        return false;
    }

    ESP_LOGI(TAG, "Connecting to SSID '%s' on channel %u", credentials_.ssid, static_cast<unsigned int>(requiredChannel_));
    return true;
}


bool WiFiManager::isConnected() const
{
    return state_.load() == WiFiConnectionState::CONNECTED_WITH_IP;
}


WiFiConnectionState WiFiManager::getState() const
{
    return state_.load();
}


std::uint8_t WiFiManager::getConnectedChannel() const
{
    if (!isConnected()) {
        return 0U;
    }

    return requiredChannel_;
}


std::uint32_t WiFiManager::getReconnectAttemptCount() const
{
    return reconnectAttempts_.load();
}


void WiFiManager::printDiagnosticReport() const
{
    const char* stateText = "UNKNOWN";
    switch (state_.load()) {
        case WiFiConnectionState::DISCONNECTED: stateText = "DISCONNECTED"; break;
        case WiFiConnectionState::SCANNING: stateText = "SCANNING"; break;
        case WiFiConnectionState::CONNECTING: stateText = "CONNECTING"; break;
        case WiFiConnectionState::CONNECTED_AWAITING_IP: stateText = "CONNECTED_AWAITING_IP"; break;
        case WiFiConnectionState::CONNECTED_WITH_IP: stateText = "CONNECTED_WITH_IP"; break;
        case WiFiConnectionState::RADIO_CHANNEL_MISMATCH: stateText = "RADIO_CHANNEL_MISMATCH"; break;
    }

    ESP_LOGI(TAG, "================ WIFI MANAGER DIAGNOSTIC ================");
    ESP_LOGI(TAG, "SSID              : %s", credentials_.ssid);
    ESP_LOGI(TAG, "Required channel  : %u", static_cast<unsigned int>(requiredChannel_));
    ESP_LOGI(TAG, "State             : %s", stateText);
    ESP_LOGI(TAG, "Reconnect attempts: %u", static_cast<unsigned int>(reconnectAttempts_.load()));
    ESP_LOGI(TAG, "==========================================================");
}


void WiFiManager::onStationDisconnected(const wifi_event_sta_disconnected_t* event)
{
    if (event != nullptr) {
        ESP_LOGW(TAG, "Wi-Fi station disconnected: reason=%u", static_cast<unsigned int>(event->reason));
    }

    if (state_.load() != WiFiConnectionState::RADIO_CHANNEL_MISMATCH) {
        state_.store(WiFiConnectionState::DISCONNECTED);
    }

    const std::uint32_t attempt = reconnectAttempts_.fetch_add(1U) + 1U;
    const std::uint32_t backoffShift = attempt < RECONNECT_BACKOFF_CAP_SHIFT ? attempt : RECONNECT_BACKOFF_CAP_SHIFT;
    std::uint64_t delayMicroseconds = RECONNECT_BASE_DELAY_MICROSECONDS << backoffShift;
    if (delayMicroseconds > RECONNECT_MAXIMUM_DELAY_MICROSECONDS) {
        delayMicroseconds = RECONNECT_MAXIMUM_DELAY_MICROSECONDS;
    }

    ESP_LOGI(TAG, "Scheduling reconnect attempt %u in %llu ms",
             static_cast<unsigned int>(attempt), static_cast<unsigned long long>(delayMicroseconds / 1000ULL));

    if (reconnectTimer != nullptr) {
        esp_timer_stop(reconnectTimer);
        esp_timer_start_once(reconnectTimer, delayMicroseconds);
    }
}


void WiFiManager::onGotIp()
{
    reconnectAttempts_.store(0U);
    state_.store(WiFiConnectionState::CONNECTED_WITH_IP);
    ESP_LOGI(TAG, "Wi-Fi connected with IP address on channel %u", static_cast<unsigned int>(requiredChannel_));
}


void WiFiManager::handleWiFiEvent(void* arg, esp_event_base_t eventBase, std::int32_t eventId, void* eventData)
{
    (void)eventBase;
    WiFiManager* self = static_cast<WiFiManager*>(arg);
    if (self == nullptr) {
        return;
    }

    if (eventId == WIFI_EVENT_STA_DISCONNECTED) {
        self->onStationDisconnected(static_cast<const wifi_event_sta_disconnected_t*>(eventData));
    } else if (eventId == WIFI_EVENT_STA_CONNECTED) {
        self->state_.store(WiFiConnectionState::CONNECTED_AWAITING_IP);
    }
}


void WiFiManager::handleIpEvent(void* arg, esp_event_base_t eventBase, std::int32_t eventId, void* eventData)
{
    (void)eventBase;
    (void)eventData;
    WiFiManager* self = static_cast<WiFiManager*>(arg);
    if (self == nullptr) {
        return;
    }

    if (eventId == IP_EVENT_STA_GOT_IP) {
        self->onGotIp();
    }
}


} // namespace kilowatts
