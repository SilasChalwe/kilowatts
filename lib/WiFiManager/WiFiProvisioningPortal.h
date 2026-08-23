/**
 * @file WiFiProvisioningPortal.h
 * @brief Declares the Central Node's self-hosted Wi-Fi setup Access Point.
 *
 * WiFiProvisioningPortal's one responsibility: when WiFiManager cannot
 * associate with the configured infrastructure Wi-Fi, stand up a Soft
 * Access Point plus a tiny captive-portal HTTP form so an installer can
 * pick and save a real SSID/password from a phone. It does not itself
 * manage station connectivity
 * (WiFiManager) and does not decide *when* to start/stop - the caller
 * (src/central/main.cpp's watchdog task) owns that policy from
 * WiFiManager::getReconnectAttemptCount()/getState().
 *
 * The Access Point is pinned to the same Wi-Fi channel ESP-NOW already
 * uses (kilowatts::KILOWATTS_RADIO_CHANNEL) so bringing it up never
 * disrupts local ESP-NOW control to the Smart Nodes - only infrastructure
 * Wi-Fi/MQTT is affected while the portal is active.
 *
 * Saved credentials are handed to WiFiCredentialsStore, then the device
 * restarts (esp_restart()) to retry station connectivity cleanly from a
 * fresh boot rather than attempting a live AP-to-STA handover in one
 * running process.
 *
 * This module requires the real ESP-IDF Wi-Fi/HTTP-server/lwIP stack and
 * is therefore ESP32-target-only, like WiFiManager - it has no host build
 * split and no host-native test.
 */

#ifndef KILOWATTS_WIFI_PROVISIONING_PORTAL_H
#define KILOWATTS_WIFI_PROVISIONING_PORTAL_H

#include <cstdint>

namespace kilowatts {


class WiFiProvisioningPortal {

public:

    WiFiProvisioningPortal();
    ~WiFiProvisioningPortal();

    WiFiProvisioningPortal(const WiFiProvisioningPortal&) = delete;
    WiFiProvisioningPortal& operator=(const WiFiProvisioningPortal&) = delete;


    /**
     * Brings up "Kilowatts-Setup-XXXX" (XXXX = last two MAC bytes, open/no
     * password) on channel, plus an HTTP form on 192.168.4.1 and a
     * captive-portal DNS responder that answers every query with the
     * portal's own address, so most phones/laptops auto-open the setup
     * page. A no-op (returns true) if already active.
     *
     * Returns false when the AP netif/Wi-Fi mode switch or the HTTP/DNS
     * servers could not be started.
     */
    bool begin(std::uint8_t channel);


    /**
     * Tears down the DNS responder, HTTP server and Access Point, and
     * restores station-only Wi-Fi mode. A no-op if not active. Does not
     * itself retry station connectivity - the caller's next WiFiManager
     * reconnect cycle handles that.
     */
    void end();


    bool isActive() const;


private:

    static void handleSavedCredentials(const char* ssid, const char* password);

    bool active_;
};


} // namespace kilowatts

#endif // KILOWATTS_WIFI_PROVISIONING_PORTAL_H
