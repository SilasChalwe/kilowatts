#include "SmartNodeConfigurationStore.h"

#include <cmath>
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "nvs.h"
#endif

namespace kilowatts {

namespace {

#ifdef ESP_PLATFORM
constexpr const char* NVS_NAMESPACE = "kw_smart_cfg";
constexpr const char* NVS_KEY_SCHEMA = "schema";
constexpr const char* NVS_KEY_LOADS = "loads";
/* Version 2 intentionally rejects the former per-load-INA219 records. */
constexpr std::uint8_t SCHEMA_VERSION = 2U;

static const char* TAG = "SMART_CFG_STORE";

#pragma pack(push, 1)
struct PersistedLoadConfiguration {
    char name[16];
    std::uint8_t relayPin;
    std::uint8_t relayActiveHigh;
    std::uint8_t mode;
    std::uint16_t priority;
    float nominalVoltageVolts;
    float nominalCurrentAmps;
    float branchMaximumCurrentAmps;
    float startupWatts;
    std::uint8_t scheduleEnabled;
    std::uint8_t scheduleHour;
    std::uint8_t scheduleMinute;
};
#pragma pack(pop)

SmartNodeConfigurationStore::LoadConfiguration fromPersisted(const PersistedLoadConfiguration& value)
{
    SmartNodeConfigurationStore::LoadConfiguration configuration{};
    std::memcpy(configuration.name, value.name, sizeof(configuration.name));
    configuration.name[sizeof(configuration.name) - 1U] = '\0';
    configuration.relayPin = value.relayPin;
    configuration.relayActiveHigh = value.relayActiveHigh != 0U;
    configuration.mode = static_cast<LoadMode::Value>(value.mode);
    configuration.priority = value.priority;
    configuration.nominalVoltageVolts = value.nominalVoltageVolts;
    configuration.nominalCurrentAmps = value.nominalCurrentAmps;
    configuration.branchMaximumCurrentAmps = value.branchMaximumCurrentAmps;
    configuration.startupWatts = value.startupWatts;
    configuration.schedule = AutoSchedule{value.scheduleEnabled != 0U, value.scheduleHour, value.scheduleMinute};
    return configuration;
}

PersistedLoadConfiguration toPersisted(const SmartNodeConfigurationStore::LoadConfiguration& value)
{
    PersistedLoadConfiguration persisted{};
    std::memcpy(persisted.name, value.name, sizeof(persisted.name));
    persisted.name[sizeof(persisted.name) - 1U] = '\0';
    persisted.relayPin = value.relayPin;
    persisted.relayActiveHigh = value.relayActiveHigh ? 1U : 0U;
    persisted.mode = static_cast<std::uint8_t>(value.mode);
    persisted.priority = value.priority;
    persisted.nominalVoltageVolts = value.nominalVoltageVolts;
    persisted.nominalCurrentAmps = value.nominalCurrentAmps;
    persisted.branchMaximumCurrentAmps = value.branchMaximumCurrentAmps;
    persisted.startupWatts = value.startupWatts;
    persisted.scheduleEnabled = value.schedule.enabled ? 1U : 0U;
    persisted.scheduleHour = value.schedule.hour;
    persisted.scheduleMinute = value.schedule.minute;
    return persisted;
}
#endif // ESP_PLATFORM

} // namespace

SmartNodeConfigurationStore::SmartNodeConfigurationStore() : configurations_() {}

bool SmartNodeConfigurationStore::isValidMode(LoadMode::Value mode)
{
    return mode == LoadMode::Fixed::OFF || mode == LoadMode::Fixed::ON ||
           mode == LoadMode::Auto::OFF || mode == LoadMode::Auto::ON;
}

std::size_t SmartNodeConfigurationStore::getNumberOfConfigurations() const
{
    return configurations_.size();
}

const SmartNodeConfigurationStore::LoadConfiguration* SmartNodeConfigurationStore::getConfiguration(std::size_t index) const
{
    return index < configurations_.size() ? &configurations_[index] : nullptr;
}

const SmartNodeConfigurationStore::LoadConfiguration* SmartNodeConfigurationStore::findByRelayPin(std::uint8_t relayPin) const
{
    for (const LoadConfiguration& configuration : configurations_) {
        if (configuration.relayPin == relayPin) {
            return &configuration;
        }
    }
    return nullptr;
}

bool SmartNodeConfigurationStore::isValidNewConfiguration(
    const LoadConfiguration& configuration, HardwareConfigurationFailureReason& failureReason) const
{
    std::size_t nameLength = 0U;
    while (nameLength < sizeof(configuration.name) && configuration.name[nameLength] != '\0') {
        ++nameLength;
    }
    if (nameLength == 0U || nameLength >= sizeof(configuration.name) ||
        !isValidMode(configuration.mode) || configuration.priority > 10U ||
        (configuration.schedule.enabled &&
         (configuration.schedule.hour > 23U || configuration.schedule.minute > 59U))) {
        failureReason = HardwareConfigurationFailureReason::INVALID_CONFIGURATION;
        return false;
    }
    const float plannedRunningWatts =
        configuration.nominalVoltageVolts * configuration.nominalCurrentAmps;
    if (!std::isfinite(configuration.nominalVoltageVolts) || configuration.nominalVoltageVolts <= 0.0F ||
        !std::isfinite(configuration.nominalCurrentAmps) || configuration.nominalCurrentAmps <= 0.0F ||
        !std::isfinite(plannedRunningWatts) || plannedRunningWatts <= 0.0F ||
        !std::isfinite(configuration.branchMaximumCurrentAmps) ||
        configuration.branchMaximumCurrentAmps <= 0.0F ||
        !std::isfinite(configuration.startupWatts) ||
        configuration.startupWatts < plannedRunningWatts) {
        failureReason = HardwareConfigurationFailureReason::INVALID_ELECTRICAL_RATING;
        return false;
    }
    if (configurations_.size() >= MAX_CONFIGURED_LOADS) {
        failureReason = HardwareConfigurationFailureReason::CAPACITY_REACHED;
        return false;
    }
    for (const LoadConfiguration& existing : configurations_) {
        if (existing.relayPin == configuration.relayPin) {
            failureReason = HardwareConfigurationFailureReason::DUPLICATE_RELAY_PIN;
            return false;
        }
    }
    failureReason = HardwareConfigurationFailureReason::NONE;
    return true;
}

bool SmartNodeConfigurationStore::applyOne(const LoadConfiguration& configuration,
                                           RelayController& relays,
                                           Node& node,
                                           HardwareConfigurationFailureReason& failureReason)
{
    if (relays.isRelayRegistered(configuration.relayPin) ||
        node.getLoadByRelayPin(configuration.relayPin) != nullptr) {
        failureReason = HardwareConfigurationFailureReason::DUPLICATE_RELAY_PIN;
        return false;
    }
    if (!relays.addRelay(RelayController::RelayConfiguration{
            configuration.relayPin, configuration.relayActiveHigh, false})) {
        failureReason = HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED;
        return false;
    }

#ifdef ESP_PLATFORM
    /*
     * RelayController deliberately retains a channel even if GPIO setup
     * fails so diagnostic callers can inspect it. An installer command is
     * stricter: it must not report APPLIED unless that physical output was
     * actually configured and driven to its safe OFF state.
     */
    if (!relays.isHardwareApplied(configuration.relayPin)) {
        relays.removeRelay(configuration.relayPin);
        failureReason = HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED;
        return false;
    }
#endif

    const float runningWatts = configuration.nominalVoltageVolts * configuration.nominalCurrentAmps;
    Load load(Load::Id{node.getMacAddress(), configuration.relayPin}, configuration.name,
              LoadPower{runningWatts, configuration.startupWatts}, configuration.priority,
              configuration.mode,
              LoadElectricalRatings{configuration.nominalVoltageVolts, configuration.nominalCurrentAmps});
    if (!node.addLoad(load)) {
        relays.removeRelay(configuration.relayPin);
        failureReason = HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED;
        return false;
    }
    Load* applied = node.getLoadByRelayPin(configuration.relayPin);
    if (applied == nullptr || !applied->setSchedule(configuration.schedule)) {
        node.removeLoadByRelayPin(configuration.relayPin);
        relays.removeRelay(configuration.relayPin);
        failureReason = HardwareConfigurationFailureReason::INVALID_CONFIGURATION;
        return false;
    }

    /*
     * addRelay() drives a newly commissioned channel to safe OFF. Record
     * that state only when GPIO read-back independently confirms it; Central
     * otherwise treats the circuit as conservatively unknown/possibly ON.
     */
    bool confirmedOff = false;
    if (relays.readBackState(configuration.relayPin, confirmedOff) && !confirmedOff) {
        applied->setConfirmedRelayState(false);
    }
    failureReason = HardwareConfigurationFailureReason::NONE;
    return true;
}

void SmartNodeConfigurationStore::rollbackOne(const LoadConfiguration& configuration,
                                              RelayController& relays,
                                              Node& node)
{
    node.removeLoadByRelayPin(configuration.relayPin);
    relays.removeRelay(configuration.relayPin);
}

bool SmartNodeConfigurationStore::configureNewLoad(const LoadConfiguration& configuration,
                                                   RelayController& relays,
                                                   Node& node,
                                                   HardwareConfigurationFailureReason& failureReason)
{
    if (!isValidNewConfiguration(configuration, failureReason)) {
        return false;
    }
    if (!applyOne(configuration, relays, node, failureReason)) {
        return false;
    }

    configurations_.push_back(configuration);
    if (!persist()) {
        configurations_.pop_back();
        rollbackOne(configuration, relays, node);
        failureReason = HardwareConfigurationFailureReason::PERSISTENCE_FAILED;
        return false;
    }
    failureReason = HardwareConfigurationFailureReason::NONE;
    return true;
}

bool SmartNodeConfigurationStore::clearAllConfigurations(
    RelayController& relays, Node& node,
    HardwareConfigurationFailureReason& failureReason)
{
    /*
     * De-energise and independently verify every channel before changing
     * durable configuration. A failed read-back must never be represented as
     * a completed decommission, even though any earlier channel may already
     * have been driven to the safer OFF state.
     */
    for (const LoadConfiguration& configuration : configurations_) {
        bool confirmedOff = false;
        if (!relays.setRelayState(configuration.relayPin, false) ||
            !relays.readBackState(configuration.relayPin, confirmedOff) || confirmedOff) {
            failureReason = HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED;
            return false;
        }
    }

    const std::vector<LoadConfiguration> previousConfigurations = configurations_;
    configurations_.clear();
    if (!persist()) {
        configurations_ = previousConfigurations;
        failureReason = HardwareConfigurationFailureReason::PERSISTENCE_FAILED;
        return false;
    }

    for (const LoadConfiguration& configuration : previousConfigurations) {
        node.removeLoadByRelayPin(configuration.relayPin);
        relays.removeRelay(configuration.relayPin);
    }

    failureReason = HardwareConfigurationFailureReason::NONE;
    return true;
}

bool SmartNodeConfigurationStore::applyPersistedConfigurations(
    RelayController& relays, Node& node,
    HardwareConfigurationFailureReason& failureReason) const
{
    std::size_t appliedCount = 0U;
    for (const LoadConfiguration& configuration : configurations_) {
        if (!applyOne(configuration, relays, node, failureReason)) {
            while (appliedCount > 0U) {
                --appliedCount;
                rollbackOne(configurations_[appliedCount], relays, node);
            }
            return false;
        }
        ++appliedCount;
    }
    failureReason = HardwareConfigurationFailureReason::NONE;
    return true;
}

bool SmartNodeConfigurationStore::branchMaximumCurrentAmps(std::uint8_t relayPin, float& value) const
{
    const LoadConfiguration* configuration = findByRelayPin(relayPin);
    if (configuration == nullptr) {
        return false;
    }
    value = configuration->branchMaximumCurrentAmps;
    return true;
}

bool SmartNodeConfigurationStore::loadPersisted()
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    std::uint8_t schema = 0U;
    std::size_t size = 0U;
    esp_err_t result = nvs_get_u8(handle, NVS_KEY_SCHEMA, &schema);
    if (result == ESP_OK && schema == SCHEMA_VERSION) {
        result = nvs_get_blob(handle, NVS_KEY_LOADS, nullptr, &size);
    }
    if (result != ESP_OK || size == 0U || size % sizeof(PersistedLoadConfiguration) != 0U) {
        nvs_close(handle);
        return false;
    }
    const std::size_t count = size / sizeof(PersistedLoadConfiguration);
    if (count > MAX_CONFIGURED_LOADS) {
        nvs_close(handle);
        return false;
    }
    std::vector<PersistedLoadConfiguration> persisted(count);
    result = nvs_get_blob(handle, NVS_KEY_LOADS, persisted.data(), &size);
    nvs_close(handle);
    if (result != ESP_OK) {
        return false;
    }

    std::vector<LoadConfiguration> restored;
    restored.reserve(count);
    for (const PersistedLoadConfiguration& entry : persisted) {
        const LoadConfiguration configuration = fromPersisted(entry);
        SmartNodeConfigurationStore validationStore;
        validationStore.configurations_ = restored;
        HardwareConfigurationFailureReason reason = HardwareConfigurationFailureReason::NONE;
        if (!validationStore.isValidNewConfiguration(configuration, reason)) {
            ESP_LOGW(TAG, "Discarding invalid persisted Smart Node configuration (reason=%u)",
                     static_cast<unsigned int>(reason));
            return false;
        }
        restored.push_back(configuration);
    }
    configurations_ = restored;
    ESP_LOGI(TAG, "Restored %u Smart Node load configuration(s)",
             static_cast<unsigned int>(configurations_.size()));
    return true;
#else
    return false;
#endif
}

bool SmartNodeConfigurationStore::persist() const
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return false;
    }
    std::vector<PersistedLoadConfiguration> persisted;
    persisted.reserve(configurations_.size());
    for (const LoadConfiguration& configuration : configurations_) {
        persisted.push_back(toPersisted(configuration));
    }
    result = nvs_set_u8(handle, NVS_KEY_SCHEMA, SCHEMA_VERSION);
    if (result == ESP_OK && persisted.empty()) {
        result = nvs_erase_key(handle, NVS_KEY_LOADS);
        if (result == ESP_ERR_NVS_NOT_FOUND) {
            result = ESP_OK;
        }
    } else if (result == ESP_OK) {
        result = nvs_set_blob(handle, NVS_KEY_LOADS, persisted.data(),
                              persisted.size() * sizeof(PersistedLoadConfiguration));
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result == ESP_OK;
#else
    return false;
#endif
}

} // namespace kilowatts
