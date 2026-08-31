/**
 * ESP-NOW and infrastructure Wi-Fi share one physical radio. This class
 * does not initialize the Wi-Fi driver; EspNowCommunication brings the
 * station interface up first. WiFiManager scans the configured SSID before
 * association. If the Access Point has moved to a different channel, the
 * newly discovered channel is persisted and Central restarts so ESP-NOW and
 * Wi-Fi come back on the same channel automatically.
 */

#ifndef KILOWATTS_WIFI_MANAGER_H
#define KILOWATTS_WIFI_MANAGER_H

#include <atomic>
#include <cstdint>

#include "esp_event.h"
#include "esp_wifi_types.h"

namespace kilowatts {


enum class WiFiConnectionState : std::uint8_t {
    DISCONNECTED = 0U,
    SCANNING = 1U,
    CONNECTING = 2U,
    CONNECTED_AWAITING_IP = 3U,
    CONNECTED_WITH_IP = 4U,
    RADIO_CHANNEL_MISMATCH = 5U
};


class WiFiManager {

public:

    static constexpr std::size_t SSID_BUFFER_SIZE = 33U;
    static constexpr std::size_t PASSWORD_BUFFER_SIZE = 65U;

    struct Credentials {
        const char* ssid;
        const char* password;
        const char* hostname;
    };


    explicit WiFiManager(std::uint8_t requiredChannel);

    /** Sets the startup shared-radio channel before begin(). */
    void setRequiredChannel(std::uint8_t requiredChannel);

    ~WiFiManager();

    WiFiManager(const WiFiManager&) = delete;
    WiFiManager& operator=(const WiFiManager&) = delete;


    /**
     * Registers Wi-Fi/IP event handlers and starts the connection sequence.
     * The configured SSID is scanned first so its current channel can be
     * verified automatically.
     */
    bool begin(const Credentials& credentials);


    bool isConnected() const;

    WiFiConnectionState getState() const;

    std::uint8_t getConnectedChannel() const;

    std::uint32_t getReconnectAttemptCount() const;

    /** Prints all named Wi-Fi networks currently visible to Central. */
    bool printNearbyNetworks() const;

    /** Finds the current channel of one SSID. */
    bool findChannelForSsid(const char* ssid, std::uint8_t& channel) const;

    bool hasInternetConnection() const;


    void printDiagnosticReport() const;


private:

    bool verifyChannelAndConnect();

    static void handleWiFiEvent(void* arg, esp_event_base_t eventBase, std::int32_t eventId, void* eventData);
    static void handleIpEvent(void* arg, esp_event_base_t eventBase, std::int32_t eventId, void* eventData);

    void onScanDone();
    void onStationDisconnected(const wifi_event_sta_disconnected_t* event);
    void onGotIp();

    std::uint8_t requiredChannel_;

    char ssid_[SSID_BUFFER_SIZE];
    char password_[PASSWORD_BUFFER_SIZE];

    std::atomic<WiFiConnectionState> state_;
    std::atomic<std::uint32_t> reconnectAttempts_;

    bool eventHandlersRegistered_;

    static WiFiManager* instance_;
};


} // namespace kilowatts

#endif // KILOWATTS_WIFI_MANAGER_H
