/**
 * @file NodeIdentityStore.cpp
 * @brief Implements a Smart Node's own local, persisted commissioning
 *        identity.
 *
 * getLifecycleState()/getFriendlyName()/applyCommission()/
 * applyDecommission() are plain, hardware-free C++ and always compiled, so
 * they are directly host-testable (see test/NodeIdentityStore/). NVS
 * persistence (loadPersisted()/persist()) is compiled only under
 * ESP_PLATFORM, matching the split LoadConfigurationStore/
 * NodeCommissioningRegistry already established.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 14 August 2026
 */

#include "NodeIdentityStore.h"

#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#endif

namespace kilowatts {


#ifdef ESP_PLATFORM
static const char *TAG = "NODE_IDENTITY_STORE";

namespace {

constexpr const char* NVS_NAMESPACE = "kw_identity";
constexpr const char* NVS_KEY_SCHEMA_VERSION = "schema";
constexpr const char* NVS_KEY_LIFECYCLE_STATE = "state";
constexpr const char* NVS_KEY_FRIENDLY_NAME = "name";
constexpr std::uint8_t CURRENT_SCHEMA_VERSION = 1U;

} // namespace
#endif // ESP_PLATFORM


namespace {

void copyTruncated(char* destination, std::size_t destinationSize, const char* source)
{
    if (destinationSize == 0U) {
        return;
    }

    std::size_t i = 0U;
    for (; source != nullptr && i < destinationSize - 1U && source[i] != '\0'; ++i) {
        destination[i] = source[i];
    }
    destination[i] = '\0';
}

bool isValidFriendlyName(const char* friendlyName)
{
    if (friendlyName == nullptr || friendlyName[0] == '\0') {
        return false;
    }

    return std::strlen(friendlyName) < NodeIdentityStore::FRIENDLY_NAME_BUFFER_SIZE;
}

} // namespace


NodeIdentityStore::NodeIdentityStore()
    : lifecycleState_(NodeLifecycleState::UNCOMMISSIONED)
    , friendlyName_{}
{
}


NodeLifecycleState NodeIdentityStore::getLifecycleState() const
{
    return lifecycleState_;
}


const char* NodeIdentityStore::getFriendlyName() const
{
    return friendlyName_;
}


bool NodeIdentityStore::applyCommission(const char* friendlyName)
{
    if (!isValidFriendlyName(friendlyName)) {
        return false;
    }

    const bool isFirstCommissioning = lifecycleState_ == NodeLifecycleState::UNCOMMISSIONED;
    const bool isRename =
        lifecycleState_ == NodeLifecycleState::COMMISSIONED ||
        lifecycleState_ == NodeLifecycleState::OPERATIONAL;

    if (!isFirstCommissioning && !isRename) {
        return false;
    }

    if (isFirstCommissioning) {
        lifecycleState_ = NodeLifecycleState::COMMISSIONED;
    }

    copyTruncated(friendlyName_, FRIENDLY_NAME_BUFFER_SIZE, friendlyName);

    return true;
}


void NodeIdentityStore::applyDecommission()
{
    lifecycleState_ = NodeLifecycleState::UNCOMMISSIONED;
    friendlyName_[0] = '\0';
}


bool NodeIdentityStore::loadPersisted()
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (result != ESP_OK) {
        return false;
    }

    std::uint8_t schemaVersion = 0U;
    result = nvs_get_u8(handle, NVS_KEY_SCHEMA_VERSION, &schemaVersion);
    if (result != ESP_OK || schemaVersion != CURRENT_SCHEMA_VERSION) {
        nvs_close(handle);
        if (result == ESP_OK) {
            ESP_LOGW(TAG, "Discarding persisted identity: unsupported schema version %u",
                     static_cast<unsigned int>(schemaVersion));
        }
        return false;
    }

    std::uint8_t rawLifecycleState = 0U;
    result = nvs_get_u8(handle, NVS_KEY_LIFECYCLE_STATE, &rawLifecycleState);
    if (result != ESP_OK) {
        nvs_close(handle);
        return false;
    }

    /*
     * UNCOMMISSIONED is a legitimate persisted value (a Node that was
     * decommissioned, or has simply never been commissioned, honestly
     * persists "no identity" rather than nothing at all) - it is not
     * treated as corrupt, and its friendly name is expected to be empty
     * rather than validated as a real name.
     */
    const NodeLifecycleState lifecycleState = static_cast<NodeLifecycleState>(rawLifecycleState);
    if (lifecycleState != NodeLifecycleState::UNCOMMISSIONED &&
        lifecycleState != NodeLifecycleState::COMMISSIONED &&
        lifecycleState != NodeLifecycleState::OPERATIONAL) {
        ESP_LOGW(TAG, "Discarding corrupt persisted identity: invalid lifecycleState %u",
                 static_cast<unsigned int>(rawLifecycleState));
        nvs_close(handle);
        return false;
    }

    char friendlyName[FRIENDLY_NAME_BUFFER_SIZE] = {};
    std::size_t friendlyNameSize = sizeof(friendlyName);
    result = nvs_get_str(handle, NVS_KEY_FRIENDLY_NAME, friendlyName, &friendlyNameSize);
    nvs_close(handle);

    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Discarding corrupt persisted identity: friendly name missing/unreadable");
        return false;
    }

    if (lifecycleState != NodeLifecycleState::UNCOMMISSIONED && !isValidFriendlyName(friendlyName)) {
        ESP_LOGW(TAG, "Discarding corrupt persisted identity: invalid friendly name");
        return false;
    }

    lifecycleState_ = lifecycleState;
    copyTruncated(friendlyName_, FRIENDLY_NAME_BUFFER_SIZE, friendlyName);

    ESP_LOGI(TAG, "Restored persisted identity: name='%s' state=%s", friendlyName_, toText(lifecycleState_));
    return true;
#else
    return false;
#endif
}


bool NodeIdentityStore::persist() const
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not open NVS to persist identity: %s", esp_err_to_name(result));
        return false;
    }

    result = nvs_set_u8(handle, NVS_KEY_SCHEMA_VERSION, CURRENT_SCHEMA_VERSION);
    if (result == ESP_OK) {
        result = nvs_set_u8(handle, NVS_KEY_LIFECYCLE_STATE, static_cast<std::uint8_t>(lifecycleState_));
    }
    if (result == ESP_OK) {
        result = nvs_set_str(handle, NVS_KEY_FRIENDLY_NAME, friendlyName_);
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }

    nvs_close(handle);

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not persist identity: %s", esp_err_to_name(result));
        return false;
    }

    ESP_LOGI(TAG, "Persisted identity: name='%s' state=%s", friendlyName_, toText(lifecycleState_));
    return true;
#else
    return false;
#endif
}


} // namespace kilowatts
