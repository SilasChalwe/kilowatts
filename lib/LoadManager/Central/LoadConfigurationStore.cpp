/**
 * @file LoadConfigurationStore.cpp
 * @brief Implements NVS persistence of user-configured Load priority,
 *        mode and Auto schedule.
 *
 * setConfiguration()/findConfiguration()/applyToLoad() are plain,
 * hardware-free C++ and always compiled, so they are directly
 * host-testable (see test/LoadConfigurationStore/). NVS persistence
 * (loadPersisted()/persist()) is compiled only under ESP_PLATFORM,
 * matching the split already used elsewhere in this project.
 */

#include "LoadConfigurationStore.h"

#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#endif

namespace kilowatts {


#ifdef ESP_PLATFORM
static const char *TAG = "LOAD_CONFIG_STORE";

namespace {

// Entries are stored as one fixed-layout record inside a single blob,
// rather than one NVS key per Load, so the remembered-Load count isn't
// limited by NVS's per-device key count.
constexpr const char* NVS_NAMESPACE = "kw_loadcfg";
constexpr const char* NVS_KEY_ENTRIES = "entries";

#pragma pack(push, 1)
struct PersistedEntryRecord {
    std::uint8_t macAddress[6];
    std::uint8_t relayPin;
    std::uint16_t priority;
    std::uint8_t mode;
    std::uint8_t scheduleEnabled;
    std::uint8_t scheduleHour;
    std::uint8_t scheduleMinute;
};
#pragma pack(pop)

} // namespace
#endif // ESP_PLATFORM


LoadConfigurationStore::LoadConfigurationStore()
    : entries_()
{
}


bool LoadConfigurationStore::isValidSchedule(const AutoSchedule& schedule)
{
    if (!schedule.enabled) {
        return true;
    }

    return schedule.hour <= 23U && schedule.minute <= 59U;
}


LoadConfigurationStore::ConfigurationEntry* LoadConfigurationStore::findMutableEntry(
    const Load::MacAddress& macAddress, std::uint8_t relayPin)
{
    for (ConfigurationEntry& entry : entries_) {
        if (entry.macAddress == macAddress && entry.relayPin == relayPin) {
            return &entry;
        }
    }

    return nullptr;
}


bool LoadConfigurationStore::setConfiguration(const ConfigurationEntry& entry)
{
    if (!isValidSchedule(entry.schedule)) {
#ifdef ESP_PLATFORM
        ESP_LOGW(TAG, "Rejected configuration for relayPin=%u: invalid schedule hour=%u minute=%u",
                 static_cast<unsigned int>(entry.relayPin),
                 static_cast<unsigned int>(entry.schedule.hour),
                 static_cast<unsigned int>(entry.schedule.minute));
#endif
        return false;
    }

    ConfigurationEntry* existing = findMutableEntry(entry.macAddress, entry.relayPin);
    if (existing != nullptr) {
        *existing = entry;
    } else {
        entries_.push_back(entry);
    }

    return true;
}


std::size_t LoadConfigurationStore::getNumberOfEntries() const
{
    return entries_.size();
}


const LoadConfigurationStore::ConfigurationEntry* LoadConfigurationStore::getEntry(std::size_t entryIndex) const
{
    if (entryIndex >= entries_.size()) {
        return nullptr;
    }

    return &entries_[entryIndex];
}


bool LoadConfigurationStore::findConfiguration(
    const Load::MacAddress& macAddress, std::uint8_t relayPin, ConfigurationEntry& entry) const
{
    for (const ConfigurationEntry& stored : entries_) {
        if (stored.macAddress == macAddress && stored.relayPin == relayPin) {
            entry = stored;
            return true;
        }
    }

    return false;
}


bool LoadConfigurationStore::applyToLoad(Load& load) const
{
    ConfigurationEntry entry{};
    if (!findConfiguration(load.getMacAddress(), load.getRelayPin(), entry)) {
        return false;
    }

    load.setPriority(entry.priority);
    load.setMode(entry.mode);

    // setSchedule() rejects the call on a Fixed Load, so a persisted
    // schedule for a Load the user has since switched to Fixed is
    // silently ignored rather than causing an error here.
    load.setSchedule(entry.schedule);

    return true;
}


bool LoadConfigurationStore::loadPersisted()
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (result != ESP_OK) {
        return false;
    }

    std::size_t blobSize = 0U;
    result = nvs_get_blob(handle, NVS_KEY_ENTRIES, nullptr, &blobSize);
    if (result != ESP_OK || blobSize == 0U || (blobSize % sizeof(PersistedEntryRecord)) != 0U) {
        nvs_close(handle);
        if (result == ESP_OK) {
            ESP_LOGW(TAG, "Discarding corrupt persisted Load configuration blob (size=%zu not a multiple of record size)",
                     blobSize);
        }
        return false;
    }

    std::vector<std::uint8_t> buffer(blobSize);
    result = nvs_get_blob(handle, NVS_KEY_ENTRIES, buffer.data(), &blobSize);
    nvs_close(handle);

    if (result != ESP_OK) {
        return false;
    }

    const std::size_t recordCount = blobSize / sizeof(PersistedEntryRecord);
    std::vector<ConfigurationEntry> restored;
    restored.reserve(recordCount);

    for (std::size_t i = 0U; i < recordCount; ++i) {
        PersistedEntryRecord record{};
        std::memcpy(&record, buffer.data() + (i * sizeof(PersistedEntryRecord)), sizeof(record));

        ConfigurationEntry entry{};
        std::memcpy(entry.macAddress.data(), record.macAddress, entry.macAddress.size());
        entry.relayPin = record.relayPin;
        entry.priority = record.priority;
        entry.mode = static_cast<LoadMode::Value>(record.mode);
        entry.schedule = AutoSchedule{
            record.scheduleEnabled != 0U,
            record.scheduleHour,
            record.scheduleMinute
        };

        if (!isValidSchedule(entry.schedule)) {
            ESP_LOGW(TAG, "Discarding corrupt persisted entry for relayPin=%u: invalid schedule",
                     static_cast<unsigned int>(entry.relayPin));
            return false;
        }

        restored.push_back(entry);
    }

    entries_ = restored;
    ESP_LOGI(TAG, "Restored %zu persisted Load configuration entries", entries_.size());
    return true;
#else
    return false;
#endif
}


bool LoadConfigurationStore::persist() const
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not open NVS to persist Load configuration: %s", esp_err_to_name(result));
        return false;
    }

    std::vector<PersistedEntryRecord> records;
    records.reserve(entries_.size());

    for (const ConfigurationEntry& entry : entries_) {
        PersistedEntryRecord record{};
        std::memcpy(record.macAddress, entry.macAddress.data(), entry.macAddress.size());
        record.relayPin = entry.relayPin;
        record.priority = entry.priority;
        record.mode = static_cast<std::uint8_t>(entry.mode);
        record.scheduleEnabled = entry.schedule.enabled ? 1U : 0U;
        record.scheduleHour = entry.schedule.hour;
        record.scheduleMinute = entry.schedule.minute;
        records.push_back(record);
    }

    result = nvs_set_blob(handle, NVS_KEY_ENTRIES, records.data(), records.size() * sizeof(PersistedEntryRecord));
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }

    nvs_close(handle);

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Could not persist Load configuration: %s", esp_err_to_name(result));
        return false;
    }

    ESP_LOGI(TAG, "Persisted %zu Load configuration entries", entries_.size());
    return true;
#else
    return false;
#endif
}


} // namespace kilowatts
