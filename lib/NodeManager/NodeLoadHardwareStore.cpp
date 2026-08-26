#include "NodeLoadHardwareStore.h"

#include <cmath>
#include <cstring>

#ifdef ESP_PLATFORM
#include "nvs.h"
#endif

namespace kilowatts {

namespace {

#ifdef ESP_PLATFORM
constexpr const char* NVS_NAMESPACE = "kw_node_loads";
constexpr const char* NVS_KEY_SCHEMA = "schema";
constexpr const char* NVS_KEY_LOADS = "loads";

/*
 * Schema 5 stores the current Load model with a complete Auto schedule
 * window (start and end time). Older start-only records are deliberately
 * not interpreted because doing so would require inventing an end time.
 */
constexpr std::uint8_t SCHEMA_VERSION = 5U;

#pragma pack(push, 1)
struct PersistedLoadConfiguration {
    char name[16];
    std::uint8_t relayPin;
    std::uint8_t relayActiveHigh;

    float powerRatingWatts;
    std::uint8_t powerType;

    std::uint8_t mode;
    std::uint16_t priority;

    std::uint8_t scheduleEnabled;
    std::uint8_t scheduleStartHour;
    std::uint8_t scheduleStartMinute;
    std::uint8_t scheduleEndHour;
    std::uint8_t scheduleEndMinute;
};
#pragma pack(pop)

NodeLoadHardwareStore::LoadConfiguration fromPersisted(
    const PersistedLoadConfiguration& value)
{
    NodeLoadHardwareStore::LoadConfiguration result{};

    std::memcpy(result.name, value.name, sizeof(result.name));
    result.name[sizeof(result.name) - 1U] = '\0';

    result.relayPin = value.relayPin;
    result.relayActiveHigh = value.relayActiveHigh != 0U;

    result.powerRatingWatts = value.powerRatingWatts;
    result.powerType = static_cast<LoadPowerType>(value.powerType);

    result.mode = static_cast<LoadMode::Value>(value.mode);
    result.priority = value.priority;

    result.schedule = AutoSchedule{
        value.scheduleEnabled != 0U,
        value.scheduleStartHour,
        value.scheduleStartMinute,
        value.scheduleEndHour,
        value.scheduleEndMinute};

    return result;
}

PersistedLoadConfiguration toPersisted(
    const NodeLoadHardwareStore::LoadConfiguration& value)
{
    PersistedLoadConfiguration result{};

    std::memcpy(result.name, value.name, sizeof(result.name));
    result.name[sizeof(result.name) - 1U] = '\0';

    result.relayPin = value.relayPin;
    result.relayActiveHigh = value.relayActiveHigh ? 1U : 0U;

    result.powerRatingWatts = value.powerRatingWatts;
    result.powerType = static_cast<std::uint8_t>(value.powerType);

    result.mode = static_cast<std::uint8_t>(value.mode);
    result.priority = value.priority;

    result.scheduleEnabled = value.schedule.enabled ? 1U : 0U;
    result.scheduleStartHour = value.schedule.startHour;
    result.scheduleStartMinute = value.schedule.startMinute;
    result.scheduleEndHour = value.schedule.endHour;
    result.scheduleEndMinute = value.schedule.endMinute;

    return result;
}
#endif

} // namespace


NodeLoadHardwareStore::NodeLoadHardwareStore()
    : configurations_()
{
}


bool NodeLoadHardwareStore::isValidMode(LoadMode::Value mode)
{
    return
        mode == LoadMode::Fixed::OFF ||
        mode == LoadMode::Fixed::ON ||
        mode == LoadMode::Auto::OFF ||
        mode == LoadMode::Auto::ON;
}


bool NodeLoadHardwareStore::isValidPowerType(LoadPowerType powerType)
{
    return
        powerType == LoadPowerType::AC ||
        powerType == LoadPowerType::DC;
}


std::size_t NodeLoadHardwareStore::getNumberOfConfigurations() const
{
    return configurations_.size();
}


const NodeLoadHardwareStore::LoadConfiguration*
NodeLoadHardwareStore::getConfiguration(std::size_t index) const
{
    return index < configurations_.size()
        ? &configurations_[index]
        : nullptr;
}


const NodeLoadHardwareStore::LoadConfiguration*
NodeLoadHardwareStore::findByRelayPin(std::uint8_t relayPin) const
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

    while (nameLength < sizeof(value.name) &&
           value.name[nameLength] != '\0') {
        ++nameLength;
    }

    bool scheduleValid = true;

    if (value.schedule.enabled) {
        scheduleValid =
            value.schedule.startHour <= 23U &&
            value.schedule.startMinute <= 59U &&
            value.schedule.endHour <= 23U &&
            value.schedule.endMinute <= 59U;

        if (scheduleValid) {
            const std::uint16_t startMinutes =
                static_cast<std::uint16_t>(value.schedule.startHour) * 60U +
                value.schedule.startMinute;

            const std::uint16_t endMinutes =
                static_cast<std::uint16_t>(value.schedule.endHour) * 60U +
                value.schedule.endMinute;

            scheduleValid = startMinutes != endMinutes;
        }
    }

    if (nameLength == 0U ||
        nameLength >= sizeof(value.name) ||
        !isValidMode(value.mode) ||
        !isValidPowerType(value.powerType) ||
        value.priority > 10U ||
        !scheduleValid) {

        reason =
            HardwareConfigurationFailureReason::INVALID_CONFIGURATION;

        return false;
    }

    if (!std::isfinite(value.powerRatingWatts) ||
        value.powerRatingWatts < 0.0F) {

        reason =
            HardwareConfigurationFailureReason::INVALID_POWER_RATING;

        return false;
    }

    if (configurations_.size() >= MAX_CONFIGURED_LOADS) {
        reason =
            HardwareConfigurationFailureReason::CAPACITY_REACHED;

        return false;
    }

    if (findByRelayPin(value.relayPin) != nullptr) {
        reason =
            HardwareConfigurationFailureReason::DUPLICATE_RELAY_PIN;

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
    if (relays.isRelayRegistered(value.relayPin) ||
        node.getLoadByRelayPin(value.relayPin) != nullptr) {

        reason =
            HardwareConfigurationFailureReason::DUPLICATE_RELAY_PIN;

        return false;
    }

    const bool initialStateOn =
        value.mode == LoadMode::Fixed::ON ||
        value.mode == LoadMode::Auto::ON;

    if (!relays.addRelay(
            RelayController::RelayConfiguration{
                value.relayPin,
                value.relayActiveHigh,
                initialStateOn})) {

        reason =
            HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED;

        return false;
    }

#ifdef ESP_PLATFORM
    if (!relays.isHardwareApplied(value.relayPin)) {
        (void)relays.removeRelay(value.relayPin);

        reason =
            HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED;

        return false;
    }
#endif

    Load load(
        Load::Id{
            node.getMacAddress(),
            value.relayPin},
        value.name,
        value.powerRatingWatts,
        value.priority,
        value.powerType,
        value.mode);

    if (!node.addLoad(load)) {
        (void)relays.removeRelay(value.relayPin);

        reason =
            HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED;

        return false;
    }

    Load* applied =
        node.getLoadByRelayPin(value.relayPin);

    if (applied == nullptr) {
        rollbackOne(value, relays, node);

        reason =
            HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED;

        return false;
    }

    if (applied->isAuto() &&
        !applied->setSchedule(value.schedule)) {

        rollbackOne(value, relays, node);

        reason =
            HardwareConfigurationFailureReason::INVALID_CONFIGURATION;

        return false;
    }

    reason = HardwareConfigurationFailureReason::NONE;
    return true;
}


void NodeLoadHardwareStore::rollbackOne(
    const LoadConfiguration& value,
    RelayController& relays,
    Node& node)
{
    (void)node.removeLoadByRelayPin(value.relayPin);
    (void)relays.removeRelay(value.relayPin);
}


bool NodeLoadHardwareStore::configureNewLoad(
    const LoadConfiguration& value,
    RelayController& relays,
    Node& node,
    HardwareConfigurationFailureReason& reason)
{
    if (!isValidNewConfiguration(value, reason) ||
        !applyOne(value, relays, node, reason)) {

        return false;
    }

    configurations_.push_back(value);

    if (!persist()) {
        configurations_.pop_back();
        rollbackOne(value, relays, node);

        reason =
            HardwareConfigurationFailureReason::PERSISTENCE_FAILED;

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

    for (std::size_t i = 0U;
         i < configurations_.size();
         ++i) {

        if (configurations_[i].relayPin == relayPin) {
            index = i;
            break;
        }
    }

    if (index == configurations_.size()) {
        reason =
            HardwareConfigurationFailureReason::INVALID_CONFIGURATION;

        return false;
    }

#ifdef ESP_PLATFORM
    if (!relays.setRelayState(relayPin, false)) {

        reason =
            HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED;

        return false;
    }
#else
    (void)relays.setRelayState(relayPin, false);
#endif

    const std::vector<LoadConfiguration> previous =
        configurations_;

    configurations_.erase(
        configurations_.begin() +
        static_cast<std::ptrdiff_t>(index));

    if (!persist()) {
        configurations_ = previous;

        reason =
            HardwareConfigurationFailureReason::PERSISTENCE_FAILED;

        return false;
    }

    (void)node.removeLoadByRelayPin(relayPin);
    (void)relays.removeRelay(relayPin);

    reason = HardwareConfigurationFailureReason::NONE;
    return true;
}


bool NodeLoadHardwareStore::clearAllConfigurations(
    RelayController& relays,
    Node& node,
    HardwareConfigurationFailureReason& reason)
{
#ifdef ESP_PLATFORM
    for (const LoadConfiguration& value : configurations_) {
        if (!relays.setRelayState(value.relayPin, false)) {

            reason =
                HardwareConfigurationFailureReason::HARDWARE_INITIALIZATION_FAILED;

            return false;
        }
    }
#endif

    const std::vector<LoadConfiguration> previous =
        configurations_;

    configurations_.clear();

    if (!persist()) {
        configurations_ = previous;

        reason =
            HardwareConfigurationFailureReason::PERSISTENCE_FAILED;

        return false;
    }

    for (const LoadConfiguration& value : previous) {
        (void)node.removeLoadByRelayPin(value.relayPin);
        (void)relays.removeRelay(value.relayPin);
    }

    reason = HardwareConfigurationFailureReason::NONE;
    return true;
}


bool NodeLoadHardwareStore::applyPersistedConfigurations(
    RelayController& relays,
    Node& node,
    HardwareConfigurationFailureReason& reason) const
{
    std::vector<std::uint8_t> appliedPins;

    for (const LoadConfiguration& value : configurations_) {
        if (!applyOne(value, relays, node, reason)) {
            for (std::uint8_t relayPin : appliedPins) {
                (void)node.removeLoadByRelayPin(relayPin);
                (void)relays.removeRelay(relayPin);
            }

            return false;
        }

        appliedPins.push_back(value.relayPin);
    }

    reason = HardwareConfigurationFailureReason::NONE;
    return true;
}


bool NodeLoadHardwareStore::loadPersisted()
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;

    if (nvs_open(
            NVS_NAMESPACE,
            NVS_READONLY,
            &handle) != ESP_OK) {

        return false;
    }

    std::uint8_t schema = 0U;

    esp_err_t result =
        nvs_get_u8(
            handle,
            NVS_KEY_SCHEMA,
            &schema);

    if (result != ESP_OK ||
        schema != SCHEMA_VERSION) {

        nvs_close(handle);
        return false;
    }

    std::size_t bytes = 0U;

    result =
        nvs_get_blob(
            handle,
            NVS_KEY_LOADS,
            nullptr,
            &bytes);

    if (result == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        configurations_.clear();
        return true;
    }

    if (result != ESP_OK ||
        bytes % sizeof(PersistedLoadConfiguration) != 0U) {

        nvs_close(handle);
        return false;
    }

    const std::size_t count =
        bytes / sizeof(PersistedLoadConfiguration);

    if (count > MAX_CONFIGURED_LOADS) {
        nvs_close(handle);
        return false;
    }

    std::vector<PersistedLoadConfiguration> records(count);

    if (bytes > 0U) {
        result =
            nvs_get_blob(
                handle,
                NVS_KEY_LOADS,
                records.data(),
                &bytes);
    }

    nvs_close(handle);

    if (result != ESP_OK) {
        return false;
    }

    std::vector<LoadConfiguration> restored;
    restored.reserve(count);

    for (const PersistedLoadConfiguration& record : records) {
        const LoadConfiguration configuration =
            fromPersisted(record);

        NodeLoadHardwareStore validator;

        HardwareConfigurationFailureReason reason =
            HardwareConfigurationFailureReason::NONE;

        if (!validator.isValidNewConfiguration(
                configuration,
                reason)) {

            return false;
        }

        validator.configurations_.push_back(configuration);
        restored.push_back(configuration);
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

    esp_err_t result =
        nvs_open(
            NVS_NAMESPACE,
            NVS_READWRITE,
            &handle);

    if (result != ESP_OK) {
        return false;
    }

    result =
        nvs_set_u8(
            handle,
            NVS_KEY_SCHEMA,
            SCHEMA_VERSION);

    if (result == ESP_OK) {
        if (configurations_.empty()) {
            const esp_err_t eraseResult =
                nvs_erase_key(
                    handle,
                    NVS_KEY_LOADS);

            if (eraseResult != ESP_OK &&
                eraseResult != ESP_ERR_NVS_NOT_FOUND) {

                result = eraseResult;
            }
        } else {
            std::vector<PersistedLoadConfiguration> records;
            records.reserve(configurations_.size());

            for (const LoadConfiguration& configuration : configurations_) {
                records.push_back(
                    toPersisted(configuration));
            }

            result =
                nvs_set_blob(
                    handle,
                    NVS_KEY_LOADS,
                    records.data(),
                    records.size() *
                        sizeof(PersistedLoadConfiguration));
        }
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
