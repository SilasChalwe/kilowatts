/**
 * @file WiFiCredentialsStore.h
 * @brief Declares NVS persistence for an installer-provisioned Wi-Fi
 *        SSID/password on the Central Node.
 *
 * WiFiCredentialsStore's one responsibility: remember the SSID/password an
 * installer entered through WiFiProvisioningPortal's captive portal, across
 * a reboot. It does not connect to Wi-Fi itself (WiFiManager), does not run
 * an Access Point or HTTP server (WiFiProvisioningPortal) - it only reads
 * and writes the credential bytes.
 *
 * There is no compiled-in Wi-Fi fallback; a never-provisioned Central Node
 * starts the setup portal instead.
 */

#ifndef KILOWATTS_WIFI_CREDENTIALS_STORE_H
#define KILOWATTS_WIFI_CREDENTIALS_STORE_H

#include <cstddef>

namespace kilowatts {


class WiFiCredentialsStore {

public:

    static constexpr std::size_t SSID_BUFFER_SIZE = 33U;      // IEEE 802.11 max SSID + null
    static constexpr std::size_t PASSWORD_BUFFER_SIZE = 65U;  // WPA2-PSK max passphrase + null

    struct Credentials {
        char ssid[SSID_BUFFER_SIZE];
        char password[PASSWORD_BUFFER_SIZE];
    };


    /**
     * Loads a previously-provisioned SSID/password from NVS into
     * credentials. Returns false (credentials left untouched) on a host
     * build, when nothing has ever been saved, or when the saved SSID is
     * empty.
     */
    bool load(Credentials& credentials) const;


    /**
     * Persists ssid/password to NVS, overwriting whatever was saved
     * before. Returns false when ssid is empty, either string does not
     * fit its buffer size above, or the underlying NVS write failed
     * (ESP32 target only - always false on a host build).
     */
    bool save(const char* ssid, const char* password) const;


    /** True when a non-empty SSID has been provisioned (ESP32 target only). */
    bool isConfigured() const;


    /** Erases any saved credentials; the next boot starts the setup portal. */
    bool clear() const;
};


} // namespace kilowatts

#endif // KILOWATTS_WIFI_CREDENTIALS_STORE_H
