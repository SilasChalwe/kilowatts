/**
 * @file LoadConfigurationStore.cpp
 * @brief Implements NVS persistence of user-configured Load priority,
 *        mode and Auto schedule.
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

constexpr const char* NVS_NAMESPACE = "kw_loadcfg";
constexpr const char* NVS_KEY_ENTRIES = "entries_v2";
constexpr const char* NVS_KEY_LEGACY_ENTRIES = "entries";

#pragma pack(push, 1)
struct PersistedEntryRecord {
    std::uint8_t macAddress[6];
    std::uint8_t relayPin;
    std::uint16_t priority;
    std::uint8_t mode;
    std::uint8_t scheduleEnabled;
    std::uint8_t scheduleStartHour;
    std::uint8_t scheduleStartMinute;
    std::uint8_t scheduleEndHour;
    std::uint8_t scheduleEndMinute;
};

struct LegacyPersistedEntryRecord {
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

    if (schedule.startHour > 23U ||
        schedule.startMinute > 59U ||
        schedule.endHour > 23U ||
        schedule.endMinute > 59U) {
        return false;
    }

    const std::uint16_t startMinutes =
        static_cast<std::uint16_t>(schedule.startHour) * 60U +
        schedule.startMinute;

    const std::uint16_t endMinutes =
        static_cast<std::uint16_t>(schedule.endHour) * 60U +
        schedule.endMinute;

    return startMinutes != endMinutes;
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
        ESP_LOGW(
            TAG,
            "Rejected configuration for relayPin=%u: invalid schedule window %02u:%02u-%02u:%02u",
            static_cast<unsigned int>(entry.relayPin),
            static_cast<unsigned int>(entry.schedule.startHour),
            static_cast<unsigned int>(entry.schedule.startMinute),
            static_cast<unsigned int>(entry.schedule.endHour),
            static_cast<unsigned int>(entry.schedule.endMinute));
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

    // Fixed Loads clear and ignore schedules by design.
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

    if (result == ESP_OK && blobSize > 0U &&
        (blobSize % sizeof(PersistedEntryRecord)) == 0U) {

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
            std::memcpy(
                &record,
                buffer.data() + (i * sizeof(PersistedEntryRecord)),
                sizeof(record));

            ConfigurationEntry entry{};
            std::memcpy(entry.macAddress.data(), record.macAddress, entry.macAddress.size());
            entry.relayPin = record.relayPin;
            entry.priority = record.priority;
            entry.mode = static_cast<LoadMode::Value>(record.mode);
            entry.schedule = AutoSchedule{
                record.scheduleEnabled != 0U,
                record.scheduleStartHour,
                record.scheduleStartMinute,
                record.scheduleEndHour,
                record.scheduleEndMinute
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
    }

    // Version 1 stored only a start time. Preserve priority/mode while
    // disabling that incomplete schedule so the user can configure a
    // proper start/end window instead of silently inventing an end time.
    blobSize = 0U;
    result = nvs_get_blob(handle, NVS_KEY_LEGACY_ENTRIES, nullptr, &blobSize);

    if (result != ESP_OK || blobSize == 0U ||
        (blobSize % sizeof(LegacyPersistedEntryRecord)) != 0U) {
        nvs_close(handle);
        return false;
    }

    std::vector<std::uint8_t> legacyBuffer(blobSize);
    result = nvs_get_blob(handle, NVS_KEY_LEGACY_ENTRIES, legacyBuffer.data(), &blobSize);
    nvs_close(handle);

    if (result != ESP_OK) {
        return false;
    }

    const std::size_t recordCount = blobSize / sizeof(LegacyPersistedEntryRecord);
    std::vector<ConfigurationEntry> restored;
    restored.reserve(recordCount);

    for (std::size_t i = 0U; i < recordCount; ++i) {
        LegacyPersistedEntryRecord record{};
        std::memcpy(
            &record,
            legacyBuffer.data() + (i * sizeof(LegacyPersistedEntryRecord)),
            sizeof(record));

        ConfigurationEntry entry{};
        std::memcpy(entry.macAddress.data(), record.macAddress, entry.macAddress.size());
        entry.relayPin = record.relayPin;
        entry.priority = record.priority;
        entry.mode = static_cast<LoadMode::Value>(record.mode);
        entry.schedule = AutoSchedule{false, 0U, 0U, 0U, 0U};
        restored.push_back(entry);
    }

    entries_ = restored;
    ESP_LOGW(TAG, "Restored legacy Load configurations; old start-only schedules were disabled");
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
        record.scheduleStartHour = entry.schedule.startHour;
        record.scheduleStartMinute = entry.schedule.startMinute;
        record.scheduleEndHour = entry.schedule.endHour;
        record.scheduleEndMinute = entry.schedule.endMinute;
        records.push_back(record);
    }

    result = nvs_set_blob(
        handle,
        NVS_KEY_ENTRIES,
        records.data(),
        records.size() * sizeof(PersistedEntryRecord));

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
