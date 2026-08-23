/*
 * Lookup/lifecycle methods are plain, hardware-free C++ and always
 * compiled, so they're directly host-testable (see
 * test/NodeCommissioningRegistry/). NVS persistence (loadPersisted()/
 * persist()) compiles only under ESP_PLATFORM.
 */

#include "NodeCommissioningRegistry.h"

#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#endif

namespace kilowatts {


#ifdef ESP_PLATFORM
static const char *TAG = "NODE_COMMISSION_REGISTRY";

namespace {

/*
 * Every record is stored as one fixed-layout entry inside a single blob
 * (not one NVS key per record), guarded by an explicit schema version key
 * so an incompatible future layout is discarded rather than misread.
 */
constexpr const char* NVS_NAMESPACE = "kw_commission";
constexpr const char* NVS_KEY_SCHEMA_VERSION = "schema";
constexpr const char* NVS_KEY_RECORDS = "records";
constexpr std::uint8_t CURRENT_SCHEMA_VERSION = 2U;

#pragma pack(push, 1)
struct PersistedCommissioningRecord {
    std::uint8_t macAddress[6];
    std::uint8_t role;
    std::uint8_t lifecycleState;
    char friendlyName[NodeCommissioningRegistry::FRIENDLY_NAME_BUFFER_SIZE];
    char firmwareVersion[NodeCommissioningRegistry::FIRMWARE_VERSION_BUFFER_SIZE];
    char chipModel[NodeCommissioningRegistry::CHIP_MODEL_BUFFER_SIZE];
    std::uint8_t relayCapabilityCount;
    std::uint8_t relayPins[NodeCommissioningRegistry::MAX_RELAY_GPIO_CAPABILITIES];
};
#pragma pack(pop)

} // namespace
#endif // ESP_PLATFORM


NodeCommissioningRegistry::NodeCommissioningRegistry()
    : records_()
{
}


bool NodeCommissioningRegistry::isValidFriendlyName(const char* friendlyName)
{
    if (friendlyName == nullptr || friendlyName[0] == '\0') {
        return false;
    }

    return std::strlen(friendlyName) < FRIENDLY_NAME_BUFFER_SIZE;
}


void NodeCommissioningRegistry::copyTruncated(char* destination, std::size_t destinationSize, const char* source)
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


NodeCommissioningRegistry::CommissioningRecord* NodeCommissioningRegistry::findMutable(const MacAddress& macAddress)
{
    for (CommissioningRecord& record : records_) {
        if (record.macAddress == macAddress) {
            return &record;
        }
    }

    return nullptr;
}


bool NodeCommissioningRegistry::recordDiscovered(
    const MacAddress& macAddress, NodeRole role,
    const char* firmwareVersion, const char* chipModel, std::uint32_t nowMilliseconds)
{
    return recordDiscovered(macAddress, role, firmwareVersion, chipModel, nullptr, 0U, nowMilliseconds);
}


bool NodeCommissioningRegistry::recordDiscovered(
    const MacAddress& macAddress, NodeRole role,
    const char* firmwareVersion, const char* chipModel,
    const std::uint8_t* relayPins, std::size_t relayCapabilityCount,
    std::uint32_t nowMilliseconds)
{
    if (relayCapabilityCount > MAX_RELAY_GPIO_CAPABILITIES) {
        relayCapabilityCount = MAX_RELAY_GPIO_CAPABILITIES;
    }

    const auto copyCapabilities = [relayPins, relayCapabilityCount](CommissioningRecord& record) {
        record.relayCapabilityCount = static_cast<std::uint8_t>(relayCapabilityCount);
        record.relayPins.fill(0U);
        for (std::size_t i = 0U; relayPins != nullptr && i < relayCapabilityCount; ++i) {
            record.relayPins[i] = relayPins[i];
        }
    };

    CommissioningRecord* existing = findMutable(macAddress);

    if (existing == nullptr) {
        CommissioningRecord record{};
        record.macAddress = macAddress;
        record.role = role;
        record.lifecycleState = NodeLifecycleState::UNCOMMISSIONED;
        record.friendlyName[0] = '\0';
        record.pendingFriendlyName[0] = '\0';
        copyTruncated(record.firmwareVersion, FIRMWARE_VERSION_BUFFER_SIZE, firmwareVersion);
        copyTruncated(record.chipModel, CHIP_MODEL_BUFFER_SIZE, chipModel);
        copyCapabilities(record);
        record.discoveredAtMilliseconds = nowMilliseconds;
        record.syncState = SyncState::SYNCED;

        records_.push_back(record);
        return true;
    }

    if (existing->lifecycleState == NodeLifecycleState::DECOMMISSIONED) {
        existing->lifecycleState = NodeLifecycleState::UNCOMMISSIONED;
    }

    copyTruncated(existing->firmwareVersion, FIRMWARE_VERSION_BUFFER_SIZE, firmwareVersion);
    copyTruncated(existing->chipModel, CHIP_MODEL_BUFFER_SIZE, chipModel);
    copyCapabilities(*existing);
    existing->discoveredAtMilliseconds = nowMilliseconds;

    return false;
}


bool NodeCommissioningRegistry::updateDiagnostics(const MacAddress& macAddress, const Diagnostics& diagnostics)
{
    CommissioningRecord* existing = findMutable(macAddress);
    if (existing == nullptr) {
        return false;
    }

    existing->diagnostics = diagnostics;
    return true;
}


bool NodeCommissioningRegistry::registerSelf(
    const MacAddress& macAddress, NodeRole role, const char* friendlyName,
    const char* firmwareVersion, const char* chipModel,
    const std::uint8_t* relayPins, std::size_t relayCapabilityCount,
    std::uint32_t nowMilliseconds)
{
    if (findMutable(macAddress) != nullptr) {
        return false;
    }

    if (relayCapabilityCount > MAX_RELAY_GPIO_CAPABILITIES) {
        relayCapabilityCount = MAX_RELAY_GPIO_CAPABILITIES;
    }

    CommissioningRecord record{};
    record.macAddress = macAddress;
    record.role = role;
    record.lifecycleState = NodeLifecycleState::COMMISSIONED;
    copyTruncated(record.friendlyName, FRIENDLY_NAME_BUFFER_SIZE, friendlyName);
    record.pendingFriendlyName[0] = '\0';
    copyTruncated(record.firmwareVersion, FIRMWARE_VERSION_BUFFER_SIZE, firmwareVersion);
    copyTruncated(record.chipModel, CHIP_MODEL_BUFFER_SIZE, chipModel);
    record.relayCapabilityCount = static_cast<std::uint8_t>(relayCapabilityCount);
    record.relayPins.fill(0U);
    for (std::size_t i = 0U; relayPins != nullptr && i < relayCapabilityCount; ++i) {
        record.relayPins[i] = relayPins[i];
    }
    record.discoveredAtMilliseconds = nowMilliseconds;
    record.syncState = SyncState::SYNCED;

    records_.push_back(record);
    return true;
}


bool NodeCommissioningRegistry::requestCommissioning(const MacAddress& macAddress, const char* friendlyName,
                                                       std::uint32_t commandId)
{
    if (!isValidFriendlyName(friendlyName)) {
        return false;
    }

    CommissioningRecord* record = findMutable(macAddress);
    if (record == nullptr) {
        return false;
    }

    const bool isFirstCommissioning =
        record->lifecycleState == NodeLifecycleState::UNCOMMISSIONED ||
        record->lifecycleState == NodeLifecycleState::DISCOVERED;
    const bool isRename =
        record->lifecycleState == NodeLifecycleState::COMMISSIONED ||
        record->lifecycleState == NodeLifecycleState::OPERATIONAL;

    if (!isFirstCommissioning && !isRename) {
        return false;
    }

    if (isFirstCommissioning &&
        !isValidNodeLifecycleTransition(record->lifecycleState, NodeLifecycleState::CONFIGURING)) {
        return false;
    }

    if (isFirstCommissioning) {
        record->lifecycleState = NodeLifecycleState::CONFIGURING;
    }

    copyTruncated(record->pendingFriendlyName, FRIENDLY_NAME_BUFFER_SIZE, friendlyName);
    record->pendingCommandId = commandId;
    record->syncState = SyncState::PENDING;

    return true;
}


bool NodeCommissioningRegistry::applyCommissionResult(
    const MacAddress& macAddress, std::uint32_t commandId, bool success, NodeLifecycleState resultingState)
{
    CommissioningRecord* record = findMutable(macAddress);
    if (record == nullptr) {
        return false;
    }

    if (record->syncState != SyncState::PENDING || record->pendingCommandId != commandId) {
        return false;
    }

    record->lifecycleState = resultingState;

    if (success) {
        copyTruncated(record->friendlyName, FRIENDLY_NAME_BUFFER_SIZE, record->pendingFriendlyName);
        record->syncState = SyncState::SYNCED;
    } else {
        record->syncState = SyncState::FAILED;
    }

    record->pendingFriendlyName[0] = '\0';

    return true;
}


bool NodeCommissioningRegistry::decommission(const MacAddress& macAddress)
{
    CommissioningRecord* record = findMutable(macAddress);
    if (record == nullptr) {
        return false;
    }

    if (!isValidNodeLifecycleTransition(record->lifecycleState, NodeLifecycleState::DECOMMISSIONED)) {
        return false;
    }

    record->lifecycleState = NodeLifecycleState::DECOMMISSIONED;
    record->friendlyName[0] = '\0';
    record->pendingFriendlyName[0] = '\0';
    record->syncState = SyncState::SYNCED;

    return true;
}


std::size_t NodeCommissioningRegistry::getCount() const
{
    return records_.size();
}


const NodeCommissioningRegistry::CommissioningRecord* NodeCommissioningRegistry::getRecord(std::size_t recordIndex) const
{
    if (recordIndex >= records_.size()) {
        return nullptr;
    }

    return &records_[recordIndex];
}


const NodeCommissioningRegistry::CommissioningRecord* NodeCommissioningRegistry::findByMac(const MacAddress& macAddress) const
{
    for (const CommissioningRecord& record : records_) {
        if (record.macAddress == macAddress) {
            return &record;
        }
    }

    return nullptr;
}


bool NodeCommissioningRegistry::loadPersisted()
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
            ESP_LOGW(TAG, "Discarding persisted commissioning records: unsupported schema version %u",
                     static_cast<unsigned int>(schemaVersion));
        }
        return false;
    }

    std::size_t blobSize = 0U;
    result = nvs_get_blob(handle, NVS_KEY_RECORDS, nullptr, &blobSize);
    if (result != ESP_OK || blobSize == 0U || (blobSize % sizeof(PersistedCommissioningRecord)) != 0U) {
        nvs_close(handle);
        if (result == ESP_OK) {
            ESP_LOGW(TAG, "Discarding corrupt persisted commissioning blob (size=%zu not a multiple of record size)",
                     blobSize);
        }
        return false;
    }

    std::vector<std::uint8_t> buffer(blobSize);
    result = nvs_get_blob(handle, NVS_KEY_RECORDS, buffer.data(), &blobSize);
    nvs_close(handle);

    if (result != ESP_OK) {
        return false;
    }

    const std::size_t recordCount = blobSize / sizeof(PersistedCommissioningRecord);
    std::vector<CommissioningRecord> restored;
    restored.reserve(recordCount);

    for (std::size_t i = 0U; i < recordCount; ++i) {
        PersistedCommissioningRecord persisted{};
        std::memcpy(&persisted, buffer.data() + (i * sizeof(PersistedCommissioningRecord)), sizeof(persisted));

        const NodeLifecycleState lifecycleState = static_cast<NodeLifecycleState>(persisted.lifecycleState);
        if (lifecycleState != NodeLifecycleState::COMMISSIONED &&
            lifecycleState != NodeLifecycleState::OPERATIONAL &&
            lifecycleState != NodeLifecycleState::DECOMMISSIONED) {
            ESP_LOGW(TAG, "Discarding corrupt persisted commissioning record: invalid lifecycleState %u",
                     static_cast<unsigned int>(persisted.lifecycleState));
            return false;
        }

        CommissioningRecord record{};
        std::memcpy(record.macAddress.data(), persisted.macAddress, record.macAddress.size());
        record.role = static_cast<NodeRole>(persisted.role);
        record.lifecycleState = lifecycleState;
        copyTruncated(record.friendlyName, FRIENDLY_NAME_BUFFER_SIZE, persisted.friendlyName);
        record.pendingFriendlyName[0] = '\0';
        copyTruncated(record.firmwareVersion, FIRMWARE_VERSION_BUFFER_SIZE, persisted.firmwareVersion);
        copyTruncated(record.chipModel, CHIP_MODEL_BUFFER_SIZE, persisted.chipModel);
        if (persisted.relayCapabilityCount > MAX_RELAY_GPIO_CAPABILITIES) {
            ESP_LOGW(TAG, "Discarding corrupt persisted commissioning record: invalid relay capability count %u",
                     static_cast<unsigned int>(persisted.relayCapabilityCount));
            return false;
        }
        record.relayCapabilityCount = persisted.relayCapabilityCount;
        record.relayPins.fill(0U);
        for (std::size_t pinIndex = 0U; pinIndex < record.relayCapabilityCount; ++pinIndex) {
            record.relayPins[pinIndex] = persisted.relayPins[pinIndex];
        }
        record.discoveredAtMilliseconds = 0U;
        record.syncState = SyncState::SYNCED;

        restored.push_back(record);
    }

    records_ = restored;
    ESP_LOGI(TAG, "Restored %zu persisted commissioning records", records_.size());
    return true;
#else
    return false;
#endif
}


bool NodeCommissioningRegistry::persist() const
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not open NVS to persist commissioning records: %s", esp_err_to_name(result));
        return false;
    }

    std::vector<PersistedCommissioningRecord> records;
    records.reserve(records_.size());

    for (const CommissioningRecord& record : records_) {
        if (record.lifecycleState != NodeLifecycleState::COMMISSIONED &&
            record.lifecycleState != NodeLifecycleState::OPERATIONAL &&
            record.lifecycleState != NodeLifecycleState::DECOMMISSIONED) {
            continue;
        }

        PersistedCommissioningRecord persisted{};
        std::memcpy(persisted.macAddress, record.macAddress.data(), record.macAddress.size());
        persisted.role = static_cast<std::uint8_t>(record.role);
        persisted.lifecycleState = static_cast<std::uint8_t>(record.lifecycleState);
        copyTruncated(persisted.friendlyName, sizeof(persisted.friendlyName), record.friendlyName);
        copyTruncated(persisted.firmwareVersion, sizeof(persisted.firmwareVersion), record.firmwareVersion);
        copyTruncated(persisted.chipModel, sizeof(persisted.chipModel), record.chipModel);
        persisted.relayCapabilityCount = record.relayCapabilityCount;
        for (std::size_t pinIndex = 0U; pinIndex < MAX_RELAY_GPIO_CAPABILITIES; ++pinIndex) {
            persisted.relayPins[pinIndex] = record.relayPins[pinIndex];
        }
        records.push_back(persisted);
    }

    result = nvs_set_u8(handle, NVS_KEY_SCHEMA_VERSION, CURRENT_SCHEMA_VERSION);
    if (result == ESP_OK) {
        result = nvs_set_blob(handle, NVS_KEY_RECORDS, records.data(), records.size() * sizeof(PersistedCommissioningRecord));
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }

    nvs_close(handle);

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not persist commissioning records: %s", esp_err_to_name(result));
        return false;
    }

    ESP_LOGI(TAG, "Persisted %zu commissioning records", records.size());
    return true;
#else
    return false;
#endif
}


} // namespace kilowatts
