#include "WiFiCredentialsStore.h"

#include <cstdio>
#include <cstring>

#ifdef ESP_PLATFORM
#include "nvs.h"
#endif

namespace kilowatts {

namespace {

constexpr const char* NVS_NAMESPACE = "kw_wifi_cred";
constexpr const char* NVS_KEY_SSID = "ssid";
constexpr const char* NVS_KEY_PASSWORD = "password";

} // namespace


bool WiFiCredentialsStore::load(Credentials& credentials) const
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    char ssid[SSID_BUFFER_SIZE] = {};
    char password[PASSWORD_BUFFER_SIZE] = {};
    std::size_t ssidLength = sizeof(ssid);
    std::size_t passwordLength = sizeof(password);

    const esp_err_t ssidResult = nvs_get_str(handle, NVS_KEY_SSID, ssid, &ssidLength);
    const esp_err_t passwordResult = nvs_get_str(handle, NVS_KEY_PASSWORD, password, &passwordLength);
    nvs_close(handle);

    if (ssidResult != ESP_OK || ssid[0] == '\0') {
        return false;
    }

    std::snprintf(credentials.ssid, sizeof(credentials.ssid), "%s", ssid);
    std::snprintf(credentials.password, sizeof(credentials.password), "%s",
                   passwordResult == ESP_OK ? password : "");
    return true;
#else
    (void)credentials;
    return false;
#endif
}


bool WiFiCredentialsStore::save(const char* ssid, const char* password) const
{
    if (ssid == nullptr || ssid[0] == '\0' || std::strlen(ssid) >= SSID_BUFFER_SIZE) {
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

    bool ok = nvs_set_str(handle, NVS_KEY_SSID, ssid) == ESP_OK;
    ok = ok && nvs_set_str(handle, NVS_KEY_PASSWORD, password != nullptr ? password : "") == ESP_OK;
    ok = ok && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
#else
    (void)password;
    return false;
#endif
}


bool WiFiCredentialsStore::isConfigured() const
{
    Credentials credentials{};
    return load(credentials);
}


bool WiFiCredentialsStore::clear() const
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
