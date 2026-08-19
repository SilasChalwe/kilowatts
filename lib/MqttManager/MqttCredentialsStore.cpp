#include "MqttCredentialsStore.h"

#include <cstdio>
#include <cstring>

#ifdef ESP_PLATFORM
#include "nvs.h"
#endif

namespace kilowatts {

namespace {

constexpr const char* NVS_NAMESPACE = "kw_mqtt_cred";
constexpr const char* NVS_KEY_HOST = "host";
constexpr const char* NVS_KEY_PORT = "port";
constexpr const char* NVS_KEY_USE_TLS = "useTls";
constexpr const char* NVS_KEY_USERNAME = "username";
constexpr const char* NVS_KEY_PASSWORD = "password";

} // namespace


bool MqttCredentialsStore::load(Credentials& credentials) const
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    char host[HOST_BUFFER_SIZE] = {};
    char username[USERNAME_BUFFER_SIZE] = {};
    char password[PASSWORD_BUFFER_SIZE] = {};
    std::size_t hostLength = sizeof(host);
    std::size_t usernameLength = sizeof(username);
    std::size_t passwordLength = sizeof(password);
    std::uint16_t port = 0U;
    std::uint8_t useTls = 0U;

    const esp_err_t hostResult = nvs_get_str(handle, NVS_KEY_HOST, host, &hostLength);
    const esp_err_t portResult = nvs_get_u16(handle, NVS_KEY_PORT, &port);
    const esp_err_t useTlsResult = nvs_get_u8(handle, NVS_KEY_USE_TLS, &useTls);
    const esp_err_t usernameResult = nvs_get_str(handle, NVS_KEY_USERNAME, username, &usernameLength);
    const esp_err_t passwordResult = nvs_get_str(handle, NVS_KEY_PASSWORD, password, &passwordLength);
    nvs_close(handle);

    if (hostResult != ESP_OK || host[0] == '\0' || portResult != ESP_OK || port == 0U) {
        return false;
    }

    std::snprintf(credentials.host, sizeof(credentials.host), "%s", host);
    credentials.port = port;
    credentials.useTls = useTlsResult == ESP_OK && useTls != 0U;
    std::snprintf(credentials.username, sizeof(credentials.username), "%s",
                   usernameResult == ESP_OK ? username : "");
    std::snprintf(credentials.password, sizeof(credentials.password), "%s",
                   passwordResult == ESP_OK ? password : "");
    return true;
#else
    (void)credentials;
    return false;
#endif
}


bool MqttCredentialsStore::save(const char* host, std::uint16_t port, bool useTls,
                                 const char* username, const char* password) const
{
    if (host == nullptr || host[0] == '\0' || std::strlen(host) >= HOST_BUFFER_SIZE || port == 0U) {
        return false;
    }
    if (username != nullptr && std::strlen(username) >= USERNAME_BUFFER_SIZE) {
        return false;
    }
    if (password != nullptr && std::strlen(password) >= PASSWORD_BUFFER_SIZE) {
        return false;
    }

#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    bool ok = nvs_set_str(handle, NVS_KEY_HOST, host) == ESP_OK;
    ok = ok && nvs_set_u16(handle, NVS_KEY_PORT, port) == ESP_OK;
    ok = ok && nvs_set_u8(handle, NVS_KEY_USE_TLS, useTls ? 1U : 0U) == ESP_OK;
    ok = ok && nvs_set_str(handle, NVS_KEY_USERNAME, username != nullptr ? username : "") == ESP_OK;
    ok = ok && nvs_set_str(handle, NVS_KEY_PASSWORD, password != nullptr ? password : "") == ESP_OK;
    ok = ok && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
#else
    (void)useTls;
    (void)username;
    (void)password;
    return false;
#endif
}


bool MqttCredentialsStore::isConfigured() const
{
    Credentials credentials{};
    return load(credentials);
}


bool MqttCredentialsStore::clear() const
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    nvs_erase_all(handle);
    const bool ok = nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
#else
    return false;
#endif
}


} // namespace kilowatts
