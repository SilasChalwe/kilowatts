#include "NodeLoadHardwareStore.h"

#include <cmath>
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "nvs.h"
#endif

namespace kilowatts {

namespace {

#ifdef ESP_PLATFORM
constexpr const char* NVS_NAMESPACE = "kw_node_loads";
constexpr const char* NVS_KEY_SCHEMA = "schema";
constexpr const char* NVS_KEY_LOADS = "loads";
constexpr std::uint8_t SCHEMA_VERSION = 3U;
static const char* TAG = "NODE_LOAD_STORE";

#pragma pack(push, 1)
struct PersistedLoadConfiguration {
    char name[16];
    std::uint8_t relayPin;
    std::uint8_t relayActiveHigh;
    std::uint8_t mode;
    std::uint16_t priority;
    float nominalVoltageVolts;
    float nominalCurrentAmps;
    float startupWatts;
    std::uint8_t scheduleEnabled;
    std::uint8_t scheduleHour;
    std::uint8_t scheduleMinute;
};
#pragma pack(pop)

NodeLoadHardwareStore::LoadConfiguration fromPersisted(const PersistedLoadConfiguration& value)
{
    NodeLoadHardwareStore::LoadConfiguration result{};
    std::memcpy(result.name, value.name, sizeof(result.name));
    result.name[sizeof(result.name) - 1U] = '\0';
    result.relayPin = value.relayPin;
    result.relayActiveHigh = value.relayActiveHigh != 0U;
    result.mode = static_cast<LoadMode::Value>(value.mode);
    result.priority = value.priority;
    result.nominalVoltageVolts = value.nominalVoltageVolts;
    result.nominalCurrentAmps = value.nominalCurrentAmps;
    result.startupWatts = value.startupWatts;
    result.schedule = AutoSchedule{
        value.scheduleEnabled != 0U,
        value.scheduleHour,
        value.scheduleMinute};
    return result;
}

PersistedLoadConfiguration toPersisted(const NodeLoadHardwareStore::LoadConfiguration& value)
{
    PersistedLoadConfiguration result{};
    std::memcpy(result.name, value.name, sizeof(result.name));
    result.name[sizeof(result.name) - 1U] = '\0';
    result.relayPin = value.relayPin;
    result.relayActiveHigh = value.relayActiveHigh ? 1U : 0U;
    result.mode = static_cast<std::uint8_t>(value.mode);
    result.priority = value.priority;
    result.nominalVoltageVolts = value.nominalVoltageVolts;
    result.nominalCurrentAmps = value.nominalCurrentAmps;
    result.startupWatts = value.startupWatts;
    result.scheduleEnabled = value.schedule.enabled ? 1U : 0U;
    result.scheduleHour = value.schedule.hour;
    result.scheduleMinute = value.schedule.minute;
    return result;
}
#endif

} // namespace

NodeLoadHardwareStore::NodeLoadHardwareStore() : configurations_() {}

bool NodeLoadHardwareStore::isValidMode(LoadMode::Value mode)
{
    return mode == LoadMode::Fixed::OFF || mode == LoadMode::Fixed::ON ||
           mode == LoadMode::Auto::OFF || mode == LoadMode::Auto::ON;
}

std::size_t NodeLoadHardwareStore::getNumberOfConfigurations() const
{
    return configurations_.size();
}

const NodeLoadHardwareStore::LoadConfiguration* NodeLoadHardwareStore::getConfiguration(std::size_t index) const
{
    return index < configurations_.size() ? &configurations_[index] : nullptr;
}

const NodeLoadHardwareStore::LoadConfiguration* NodeLoadHardwareStore::findByRelayPin(std::uint8_t relayPin) const
{
    for (const LoadConfiguration& configuration : configurations_) {
        if (configuration.relayPin == relayPin) {
            return &configuration;
        }
    }
    return nullptr;
}

bool NodeLoadHardwareStore::isValidNewConfiguration(
    const LoadConfiguration& value,
    HardwareConfigurationFailureReason& reason) const
{
    std::size_t nameLength = 0U;
    while (nameLength < sizeof(value.name) && value.name[nameLength] != '\0') {
        ++nameLength;
    }

    const float runningWatts = value.nominalVoltageVolts * value.nominalCurrentAmps;

    if (nameLength == 0U || nameLength >= sizeof(value.name) ||
        !isValidMode(value.mode) || value.priority > 10U ||
        (value.schedule.enabled && (value.schedule.hour > 23U || value.schedule.minute > 59U))) {
        reason = HardwareConfigurationFailureReason::INVALID_CONFIGURATION;
        return false;
    }

    if (!std::isfinite(value.nominalVoltageVolts) || value.nominalVoltageVolts <= 0.0F ||
        !std::isfinite(value.nominalCurrentAmps) || value.nominalCurrentAmps <= 0.0F ||
        !std::isfinite(runningWatts) || runningWatts <= 0.0F ||
        !std::isfinite(value.startupWatts) || value.startupWatts < runningWatts) {
        reason = HardwareConfigurationFailureReason::INVALID_ELECTRICAL_RATING;
        return false;
    }

    if (configurations_.size() >= MAX_CONFIGURED_LOADS) {
        reason = HardwareConfigurationFailureReason::CAPACITY_REACHED;
        return false;
    }

    if (findByRelayPin(value.relayPin) != nullptr) {
        reason = HardwareConfigurationFailureReason::DUPLICATE_RELAY_PIN;
        return false;
    }

    reason = HardwareConfigurationFailureReason::NONE;
    return true;
}

bool NodeLoadHardwareStore::applyOne(
    const LoadConfiguration& value,
    RelayController& relays,
    Node& node,
    HardwareConfigurationFailureReason& reason)
{
    if (relays.isRelayRegistered(value.relayPin) || node.getLoadByRelayPin(value.relayPin) != nullptr) {
        reason = HardwareConfigurationFailureReason::DUPLICATE_RELAY_PIN;
        return false;
    }

    if (!relays.addRelay(RelayController::RelayConfiguration{value.relayPin, value.relayActiveHigh, false})) {
        reason = HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED;
        return false;
    }

#ifdef ESP_PLATFORM
    if (!relays.isHardwareApplied(value.relayPin)) {
        relays.removeRelay(value.relayPin);
        reason = HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED;
        return false;
    }
#endif

    const float runningWatts = value.nominalVoltageVolts * value.nominalCurrentAmps;
    Load load(
        Load::Id{node.getMacAddress(), value.relayPin},
        value.name,
        LoadPower{runningWatts, value.startupWatts},
        value.priority,
        value.mode,
        LoadElectricalRatings{value.nominalVoltageVolts, value.nominalCurrentAmps});

    if (!node.addLoad(load)) {
        relays.removeRelay(value.relayPin);
        reason = HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED;
        return false;
    }

    Load* applied = node.getLoadByRelayPin(value.relayPin);
    if (applied == nullptr) {
        rollbackOne(value, relays, node);
        reason = HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED;
        return false;
    }

    if (applied->isAuto() && !applied->setSchedule(value.schedule)) {
        rollbackOne(value, relays, node);
        reason = HardwareConfigurationFailureReason::INVALID_CONFIGURATION;
        return false;
    }

    bool pinOn = false;
    if (!relays.readBackState(value.relayPin, pinOn)) {
        rollbackOne(value, relays, node);
        reason = HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED;
        return false;
    }

    applied->setConfirmedRelayState(pinOn);
    applied->setHealth(LoadHealth::AVAILABLE);
    reason = HardwareConfigurationFailureReason::NONE;
    return true;
}

void NodeLoadHardwareStore::rollbackOne(
    const LoadConfiguration& value,
    RelayController& relays,
    Node& node)
{
    node.removeLoadByRelayPin(value.relayPin);
    relays.removeRelay(value.relayPin);
}

bool NodeLoadHardwareStore::configureNewLoad(
    const LoadConfiguration& value,
    RelayController& relays,
    Node& node,
    HardwareConfigurationFailureReason& reason)
{
    if (!isValidNewConfiguration(value, reason) || !applyOne(value, relays, node, reason)) {
        return false;
    }

    configurations_.push_back(value);
    if (!persist()) {
        configurations_.pop_back();
        rollbackOne(value, relays, node);
        reason = HardwareConfigurationFailureReason::PERSISTENCE_FAILED;
        return false;
    }

    reason = HardwareConfigurationFailureReason::NONE;
    return true;
}

bool NodeLoadHardwareStore::removeLoad(
    std::uint8_t relayPin,
    RelayController& relays,
    Node& node,
    HardwareConfigurationFailureReason& reason)
{
    std::size_t index = configurations_.size();
    for (std::size_t i = 0U; i < configurations_.size(); ++i) {
        if (configurations_[i].relayPin == relayPin) {
            index = i;
            break;
        }
    }

    if (index == configurations_.size()) {
        reason = HardwareConfigurationFailureReason::INVALID_CONFIGURATION;
        return false;
    }

    bool pinOn = true;
    if (!relays.setRelayState(relayPin, false) ||
        !relays.readBackState(relayPin, pinOn) || pinOn) {
        reason = HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED;
        return false;
    }

    const std::vector<LoadConfiguration> previous = configurations_;
    configurations_.erase(configurations_.begin() + static_cast<std::ptrdiff_t>(index));

    if (!persist()) {
        configurations_ = previous;
        reason = HardwareConfigurationFailureReason::PERSISTENCE_FAILED;
        return false;
    }

    node.removeLoadByRelayPin(relayPin);
    relays.removeRelay(relayPin);
    reason = HardwareConfigurationFailureReason::NONE;
    return true;
}

bool NodeLoadHardwareStore::clearAllConfigurations(
    RelayController& relays,
    Node& node,
    HardwareConfigurationFailureReason& reason)
{
    for (const LoadConfiguration& value : configurations_) {
        bool pinOn = true;
        if (!relays.setRelayState(value.relayPin, false) ||
            !relays.readBackState(value.relayPin, pinOn) || pinOn) {
            reason = HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED;
            return false;
        }
    }

    const std::vector<LoadConfiguration> previous = configurations_;
    configurations_.clear();
    if (!persist()) {
        configurations_ = previous;
        reason = HardwareConfigurationFailureReason::PERSISTENCE_FAILED;
        return false;
    }

    for (const LoadConfiguration& value : previous) {
        node.removeLoadByRelayPin(value.relayPin);
        relays.removeRelay(value.relayPin);
    }

    reason = HardwareConfigurationFailureReason::NONE;
    return true;
}

bool NodeLoadHardwareStore::applyPersistedConfigurations(
    RelayController& relays,
    Node& node,
    HardwareConfigurationFailureReason& reason) const
{
    std::size_t appliedCount = 0U;
    for (const LoadConfiguration& value : configurations_) {
        if (!applyOne(value, relays, node, reason)) {
            while (appliedCount > 0U) {
                --appliedCount;
                rollbackOne(configurations_[appliedCount], relays, node);
            }
            return false;
        }
        ++appliedCount;
    }
    reason = HardwareConfigurationFailureReason::NONE;
    return true;
}

bool NodeLoadHardwareStore::loadPersisted()
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

    std::vector<PersistedLoadConfiguration> stored(count);
    result = nvs_get_blob(handle, NVS_KEY_LOADS, stored.data(), &size);
    nvs_close(handle);
    if (result != ESP_OK) {
        return false;
    }

    std::vector<LoadConfiguration> restored;
    for (const PersistedLoadConfiguration& entry : stored) {
        const LoadConfiguration value = fromPersisted(entry);
        NodeLoadHardwareStore validator;
        validator.configurations_ = restored;
        HardwareConfigurationFailureReason validationReason = HardwareConfigurationFailureReason::NONE;
        if (!validator.isValidNewConfiguration(value, validationReason)) {
#ifdef ESP_PLATFORM
            ESP_LOGW(TAG, "Ignoring invalid persisted load on pin %u", static_cast<unsigned int>(value.relayPin));
#endif
            return false;
        }
        restored.push_back(value);
    }

    configurations_ = restored;
    return true;
#else
    return false;
#endif
}

bool NodeLoadHardwareStore::persist() const
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    std::vector<PersistedLoadConfiguration> stored;
    stored.reserve(configurations_.size());
    for (const LoadConfiguration& value : configurations_) {
        stored.push_back(toPersisted(value));
    }

    esp_err_t result = nvs_set_u8(handle, NVS_KEY_SCHEMA, SCHEMA_VERSION);
    if (result == ESP_OK && stored.empty()) {
        result = nvs_erase_key(handle, NVS_KEY_LOADS);
        if (result == ESP_ERR_NVS_NOT_FOUND) {
            result = ESP_OK;
        }
    } else if (result == ESP_OK) {
        result = nvs_set_blob(handle, NVS_KEY_LOADS, stored.data(),
                              stored.size() * sizeof(PersistedLoadConfiguration));
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result == ESP_OK;
#else
    return true;
#endif
}

} // namespace kilowatts
