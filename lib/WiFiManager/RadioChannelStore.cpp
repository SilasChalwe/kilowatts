#include "RadioChannelStore.h"

#ifdef ESP_PLATFORM
#include "nvs.h"
#endif

namespace kilowatts {

namespace {

constexpr const char* NVS_NAMESPACE = "kw_radio_ch";
constexpr const char* NVS_KEY_CHANNEL = "channel";

bool isValidChannel(std::uint8_t channel)
{
    return channel >= 1U && channel <= 14U;
}

} // namespace


bool RadioChannelStore::load(std::uint8_t& channel) const
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    std::uint8_t stored = 0U;
    const esp_err_t result = nvs_get_u8(handle, NVS_KEY_CHANNEL, &stored);
    nvs_close(handle);

    if (result != ESP_OK || !isValidChannel(stored)) {
        return false;
    }

    channel = stored;
    return true;
#else
    (void)channel;
    return false;
#endif
}


bool RadioChannelStore::save(std::uint8_t channel) const
{
    if (!isValidChannel(channel)) {
        return false;
    }

#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    bool ok = nvs_set_u8(handle, NVS_KEY_CHANNEL, channel) == ESP_OK;
    ok = ok && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
#else
    return false;
#endif
}


bool RadioChannelStore::clear() const
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
