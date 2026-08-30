/**
 * ESP-NOW and infrastructure Wi-Fi share one physical radio. This class
 * does NOT call esp_wifi_init()/esp_wifi_start()/esp_wifi_set_mode() —
 * EspNowCommunication::initialize() already brings the Wi-Fi driver up in
 * station mode on kilowatts::KILOWATTS_RADIO_CHANNEL (see
 * include/RadioConfig.h), and WiFiManager only ever associates with an
 * Access Point confirmed to already be broadcasting on that exact
 * channel. Associating on a different channel would force the whole
 * radio onto that channel and silently break ESP-NOW to every Smart
 * Node, so WiFiManager refuses to connect on a mismatch
 * (RADIO_CHANNEL_MISMATCH) rather than connecting anyway.
 */

#ifndef KILOWATTS_WIFI_MANAGER_H
#define KILOWATTS_WIFI_MANAGER_H

#include <atomic>
#include <cstdint>

#include "esp_event.h"
#include "esp_wifi_types.h"

namespace kilowatts {


/**
 * Never collapses to a single boolean, since "not connected" has several
 * distinct causes a diagnostic needs to tell apart.
 */
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

    static constexpr std::size_t SSID_BUFFER_SIZE = 33U;      // IEEE 802.11 max SSID + null
    static constexpr std::size_t PASSWORD_BUFFER_SIZE = 65U;  // WPA2-PSK max passphrase + null

    struct Credentials {
        const char* ssid;
        const char* password;

        /**
         * Cosmetic only, never part of authentication. nullptr or empty
         * leaves esp-idf's own default hostname in place.
         */
        const char* hostname;
    };


    /**
     * requiredChannel must be exactly the channel EspNowCommunication was
     * constructed with (kilowatts::KILOWATTS_RADIO_CHANNEL) — the channel
     * this Central Node's Access Point must already be broadcasting on.
     */
    explicit WiFiManager(std::uint8_t requiredChannel);

    /**
     * Overrides the channel supplied at construction. Must be called
     * before begin() — this class never re-verifies an already-running
     * connection against a new channel.
     */
    void setRequiredChannel(std::uint8_t requiredChannel);

    ~WiFiManager();

    WiFiManager(const WiFiManager&) = delete;
    WiFiManager& operator=(const WiFiManager&) = delete;


    /**
     * Registers Wi-Fi/IP event handlers and starts the connection
     * sequence (channel verification, then association). Must be called
     * after EspNowCommunication::initialize() has already brought up the
     * Wi-Fi driver in station mode — this class only registers event
     * handlers and issues esp_wifi_scan_start()/esp_wifi_connect(), it
     * never (re)initialises the Wi-Fi driver itself.
     *
     * Returns false when event handler registration itself failed, or
     * credentials.ssid is empty. This is independent of whether
     * association has actually completed yet — see getState() for that.
     */
    bool begin(const Credentials& credentials);


    /** Equivalent to getState() == CONNECTED_WITH_IP. */
    bool isConnected() const;

    WiFiConnectionState getState() const;

    /** Channel actually associated with, or 0 when not currently connected. */
    std::uint8_t getConnectedChannel() const;

    std::uint32_t getReconnectAttemptCount() const;

    /**
     * When already connected, prints the connected network only instead
     * of scanning — a real scan would retune the shared radio and
     * interrupt ESP-NOW.
     */
    bool printNearbyNetworks() const;

    /**
     * Skips scanning and reports the connected AP's channel directly when
     * already connected to ssid, to avoid retuning the shared radio — see
     * printNearbyNetworks(). Returns false when ssid is empty or not
     * currently in range.
     */
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

    /**
     * Owned copies of the SSID/password bytes, populated in begin(). The
     * caller's Credentials struct only borrows pointers (e.g. into a
     * stack-local buffer that does not outlive the call), so this class
     * must not retain those pointers past begin() returning.
     */
    char ssid_[SSID_BUFFER_SIZE];
    char password_[PASSWORD_BUFFER_SIZE];

    std::atomic<WiFiConnectionState> state_;
    std::atomic<std::uint32_t> reconnectAttempts_;

    bool eventHandlersRegistered_;

    static WiFiManager* instance_;
};


} // namespace kilowatts

#endif // KILOWATTS_WIFI_MANAGER_H
