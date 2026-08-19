/** NVS persistence for an installer-provisioned MQTT broker connection. Wins over KilowattsSecrets.h's cloud broker when present (see checkMqttStartTrigger()). */

#ifndef KILOWATTS_MQTT_CREDENTIALS_STORE_H
#define KILOWATTS_MQTT_CREDENTIALS_STORE_H

#include <cstddef>
#include <cstdint>

namespace kilowatts {


class MqttCredentialsStore {

public:

    static constexpr std::size_t HOST_BUFFER_SIZE = 128U;
    static constexpr std::size_t USERNAME_BUFFER_SIZE = 65U;
    static constexpr std::size_t PASSWORD_BUFFER_SIZE = 65U;

    struct Credentials {
        char host[HOST_BUFFER_SIZE];
        std::uint16_t port;
        bool useTls;
        char username[USERNAME_BUFFER_SIZE];
        char password[PASSWORD_BUFFER_SIZE];
    };


    /**
     * Loads a previously-provisioned broker connection from NVS into
     * credentials. Returns false (credentials left untouched) on a host
     * build, when nothing has ever been saved, or when the saved host is
     * empty.
     */
    bool load(Credentials& credentials) const;


    /**
     * Persists host/port/useTls/username/password to NVS, overwriting
     * whatever was saved before. Returns false when host is empty or
     * port is zero, any string does not fit its buffer size above, or
     * the underlying NVS write failed (ESP32 target only - always false
     * on a host build). username/password may be nullptr for a broker
     * that requires neither.
     */
    bool save(const char* host, std::uint16_t port, bool useTls,
              const char* username, const char* password) const;


    /** True when a non-empty broker host has been provisioned (ESP32 target only). */
    bool isConfigured() const;


    /** Erases any saved broker connection - MqttManager::begin() is never called again until re-provisioned. */
    bool clear() const;
};


} // namespace kilowatts

#endif // KILOWATTS_MQTT_CREDENTIALS_STORE_H
