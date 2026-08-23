/**
 * @file WiFiManager.cpp
 * @brief Implements Central Node infrastructure Wi-Fi station connectivity.
 */

#include "WiFiManager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

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
      ssid_{},
      password_{},
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
        ESP_LOGE(TAG, "begin() rejected: no SSID configured; use the setup portal");
        return false;
    }

    std::snprintf(ssid_, sizeof(ssid_), "%s", credentials.ssid);
    std::snprintf(password_, sizeof(password_), "%s",
                  credentials.password != nullptr ? credentials.password : "");

    if (credentials.hostname != nullptr && credentials.hostname[0] != '\0') {
        esp_netif_t* staNetif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (staNetif != nullptr) {
            esp_netif_set_hostname(staNetif, credentials.hostname);
        } else {
            ESP_LOGW(TAG, "wifi: station netif unavailable; DHCP hostname was not applied (hostname=%s)",
                     credentials.hostname);
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
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &WiFiManager::handleWiFiEvent);
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
    result = esp_timer_create(&timerArgs, &reconnectTimer);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create() failed: %s; automatic reconnection after a disconnect is disabled",
                 esp_err_to_name(result));
    }

    return verifyChannelAndConnect();
}


bool WiFiManager::verifyChannelAndConnect()
{
    state_.store(WiFiConnectionState::SCANNING);

    wifi_scan_config_t scanConfig{};
    scanConfig.ssid = reinterpret_cast<std::uint8_t*>(const_cast<char*>(ssid_));
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
        ESP_LOGW(TAG, "Wi-Fi scan for SSID '%s' failed to start: %s", ssid_, esp_err_to_name(result));
        onStationDisconnected(nullptr);
        return false;
    }

    std::uint16_t apCount = 0U;
    esp_wifi_scan_get_ap_num(&apCount);

    if (apCount == 0U) {
        ESP_LOGW(TAG, "SSID '%s' was not found in the scan; retrying with backoff", ssid_);
        onStationDisconnected(nullptr);
        return false;
    }

    std::vector<wifi_ap_record_t> records(apCount);
    esp_wifi_scan_get_ap_records(&apCount, records.data());

    bool found = false;
    std::uint8_t discoveredChannel = 0U;

    for (std::uint16_t i = 0U; i < apCount; ++i) {
        if (std::strncmp(reinterpret_cast<const char*>(records[i].ssid), ssid_, sizeof(records[i].ssid)) == 0) {
            found = true;
            discoveredChannel = records[i].primary;
            break;
        }
    }

    if (!found) {
        ESP_LOGW(TAG, "SSID '%s' not present among %u scanned networks; retrying with backoff",
                 ssid_, static_cast<unsigned int>(apCount));
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
                 ssid_, static_cast<unsigned int>(discoveredChannel), static_cast<unsigned int>(requiredChannel_));
        state_.store(WiFiConnectionState::RADIO_CHANNEL_MISMATCH);
        return false;
    }

    /*
     * wifi_sta_config_t::ssid/password are fixed-size raw byte fields (32
     * and 64 bytes) that ESP-IDF does not require to be null-terminated at
     * their maximum length - a full-length (32-char SSID / 64-char
     * WPA2-PSK) value has no room left for a trailing null. snprintf's
     * always-null-terminate behavior would therefore truncate the last
     * byte of a maximum-length value; memcpy of the real string length
     * (capped to the destination size) does not have that problem. The
     * struct is already zero-initialized above, so any unused tail stays
     * zero-filled.
     */
    wifi_config_t wifiConfig{};
    std::memcpy(wifiConfig.sta.ssid, ssid_, std::min(std::strlen(ssid_), sizeof(wifiConfig.sta.ssid)));
    std::memcpy(wifiConfig.sta.password, password_, std::min(std::strlen(password_), sizeof(wifiConfig.sta.password)));
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

    ESP_LOGI(TAG, "Connecting to SSID '%s' on channel %u", ssid_, static_cast<unsigned int>(requiredChannel_));
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

bool WiFiManager::printNearbyNetworks() const
{
    if (isConnected()) {
        std::printf("Wi-Fi scan skipped: do not scan while Central is connected; ESP-NOW must keep its radio channel.\n");
        return false;
    }

    wifi_scan_config_t scanConfig{};
    scanConfig.show_hidden = false;
    if (esp_wifi_scan_start(&scanConfig, true) != ESP_OK) {
        std::printf("Wi-Fi scan failed to start.\n");
        return false;
    }

    std::uint16_t count = 0U;
    if (esp_wifi_scan_get_ap_num(&count) != ESP_OK || count == 0U) {
        std::printf("Wi-Fi networks detected: 0\n");
        return true;
    }

    std::vector<wifi_ap_record_t> records(count);
    if (esp_wifi_scan_get_ap_records(&count, records.data()) != ESP_OK) {
        std::printf("Wi-Fi scan results could not be read.\n");
        return false;
    }

    std::sort(records.begin(), records.end(), [](const wifi_ap_record_t& left, const wifi_ap_record_t& right) {
        return left.rssi > right.rssi;
    });

    std::printf("WI-FI NETWORKS DETECTED\n");
    std::printf("%-4s %-32s %-8s %-8s\n", "No.", "SSID", "Signal", "Channel");
    std::size_t printed = 0U;
    for (const wifi_ap_record_t& record : records) {
        const char* ssid = reinterpret_cast<const char*>(record.ssid);
        if (ssid[0] == '\0') continue;
        std::printf("%-4u %-32s %-8d %-8u\n",
                    static_cast<unsigned int>(++printed),
                    ssid,
                    static_cast<int>(record.rssi),
                    static_cast<unsigned int>(record.primary));
    }
    if (printed == 0U) std::printf("No named Wi-Fi networks detected.\n");
    return true;
}

bool WiFiManager::hasInternetConnection() const
{
    if (!isConnected()) return false;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo("connectivitycheck.gstatic.com", "80", &hints, &result) != 0 || result == nullptr) {
        return false;
    }

    const int socketHandle = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (socketHandle < 0) {
        freeaddrinfo(result);
        return false;
    }

    timeval timeout{};
    timeout.tv_sec = 2;
    setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    const bool reachable = connect(socketHandle, result->ai_addr, result->ai_addrlen) == 0;
    close(socketHandle);
    freeaddrinfo(result);
    return reachable;
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
    ESP_LOGI(TAG, "SSID              : %s", ssid_);
    ESP_LOGI(TAG, "Required channel  : %u", static_cast<unsigned int>(requiredChannel_));
    ESP_LOGI(TAG, "State             : %s", stateText);
    ESP_LOGI(TAG, "Reconnect attempts: %u", static_cast<unsigned int>(reconnectAttempts_.load()));
    if (isConnected()) {
        wifi_ap_record_t accessPoint{};
        if (esp_wifi_sta_get_ap_info(&accessPoint) == ESP_OK) {
            ESP_LOGI(TAG, "Signal strength   : %d dBm", static_cast<int>(accessPoint.rssi));
            ESP_LOGI(TAG, "Access point      : %02X:%02X:%02X:%02X:%02X:%02X",
                     accessPoint.bssid[0], accessPoint.bssid[1], accessPoint.bssid[2],
                     accessPoint.bssid[3], accessPoint.bssid[4], accessPoint.bssid[5]);
        }
        esp_netif_t* station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ipInfo{};
        if (station != nullptr && esp_netif_get_ip_info(station, &ipInfo) == ESP_OK) {
            ESP_LOGI(TAG, "IP address        : " IPSTR, IP2STR(&ipInfo.ip));
            ESP_LOGI(TAG, "Gateway           : " IPSTR, IP2STR(&ipInfo.gw));
            ESP_LOGI(TAG, "Netmask           : " IPSTR, IP2STR(&ipInfo.netmask));
        }
    }
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
