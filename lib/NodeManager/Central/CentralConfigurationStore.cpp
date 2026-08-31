#include "CentralConfigurationStore.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

#ifdef ESP_PLATFORM
#include "nvs.h"
#endif

namespace kilowatts {
namespace {

bool isValidPercent(float value)
{
    return std::isfinite(value) && value >= 0.0F && value <= 100.0F;
}

#ifdef ESP_PLATFORM
constexpr const char* NVS_NAMESPACE = "kw_central";
constexpr const char* VERSION_KEY = "cfg_ver";
constexpr const char* DATA_KEY = "cfg";
constexpr std::uint8_t SAVED_VERSION = 8U;
constexpr std::uint8_t PREVIOUS_SAVED_VERSION = 7U;
constexpr std::uint8_t INA219_CONFIGURED_FLAG = 0x01U;
constexpr std::uint8_t BATTERY_METADATA_CONFIGURED_FLAG = 0x02U;

struct PersistedConfiguration {
    std::uint8_t batteryConfigurationFlags;
    float shuntResistanceOhms;
    float maximumExpectedCurrentAmps;
    float emaAlpha;
    float batteryCapacityAmpHours;
    float initialStateOfChargePercent;
    float nominalVoltageVolts;

    std::uint8_t powerPlanningConfigured;
    float P_budget;
    float P_reserve;
    float minimumStateOfChargePercent;
    float requiredRuntimeHours;
};
#endif
} // namespace

CentralConfigurationStore::CentralConfigurationStore()
    : configuration_{
          BatterySensorConfiguration{false, false, 0.0F, 0.0F, 0.2F, 0.0F, 0.0F, 0.0F},
          PowerPlanningConfiguration{false, 0.0F, 0.0F, 0.0F, 0.0F}}
{
}

const CentralConfigurationStore::Configuration&
CentralConfigurationStore::getConfiguration() const
{
    return configuration_;
}

bool CentralConfigurationStore::isValidBatterySensor(
    const BatterySensorConfiguration& configuration)
{
    if (configuration.ina219Configured) {
        if (!std::isfinite(configuration.shuntResistanceOhms) ||
            configuration.shuntResistanceOhms <= 0.0F ||
            !std::isfinite(configuration.maximumExpectedCurrentAmps) ||
            configuration.maximumExpectedCurrentAmps <= 0.0F ||
            !std::isfinite(configuration.emaAlpha) ||
            configuration.emaAlpha <= 0.0F || configuration.emaAlpha > 1.0F) {
            return false;
        }
    }

    if (configuration.batteryMetadataConfigured) {
        if (!std::isfinite(configuration.batteryCapacityAmpHours) ||
            configuration.batteryCapacityAmpHours <= 0.0F ||
            !isValidPercent(configuration.initialStateOfChargePercent) ||
            !std::isfinite(configuration.nominalVoltageVolts) ||
            configuration.nominalVoltageVolts <= 0.0F) {
            return false;
        }
    }

    return true;
}

bool CentralConfigurationStore::isValidPowerPlanning(
    const PowerPlanningConfiguration& configuration)
{
    if (!configuration.configured) return true;
    return std::isfinite(configuration.P_budget) && configuration.P_budget > 0.0F &&
           std::isfinite(configuration.P_reserve) && configuration.P_reserve >= 0.0F &&
           configuration.P_reserve <= configuration.P_budget &&
           isValidPercent(configuration.minimumStateOfChargePercent) &&
           std::isfinite(configuration.requiredRuntimeHours) && configuration.requiredRuntimeHours >= 0.0F;
}

bool CentralConfigurationStore::setBatterySensor(
    const BatterySensorConfiguration& configuration)
{
    if (!isValidBatterySensor(configuration)) return false;
    configuration_.batterySensor = configuration;
    return true;
}

bool CentralConfigurationStore::setPowerPlanning(
    const PowerPlanningConfiguration& configuration)
{
    if (!isValidPowerPlanning(configuration)) return false;
    configuration_.powerPlanning = configuration;
    return true;
}

bool CentralConfigurationStore::loadPersisted()
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;

    std::uint8_t version = 0U;
    esp_err_t result = nvs_get_u8(handle, VERSION_KEY, &version);
    PersistedConfiguration saved{};
    std::size_t savedSize = sizeof(saved);

    if (result == ESP_OK &&
        (version == SAVED_VERSION || version == PREVIOUS_SAVED_VERSION)) {
        result = nvs_get_blob(handle, DATA_KEY, &saved, &savedSize);
    }
    nvs_close(handle);

    if (result != ESP_OK ||
        (version != SAVED_VERSION && version != PREVIOUS_SAVED_VERSION) ||
        savedSize != sizeof(saved)) {
        return false;
    }

    bool ina219Configured = false;
    bool batteryMetadataConfigured = false;
    if (version == PREVIOUS_SAVED_VERSION) {
        const bool oldCombinedConfiguration = saved.batteryConfigurationFlags != 0U;
        ina219Configured = oldCombinedConfiguration;
        batteryMetadataConfigured = oldCombinedConfiguration;
    } else {
        ina219Configured =
            (saved.batteryConfigurationFlags & INA219_CONFIGURED_FLAG) != 0U;
        batteryMetadataConfigured =
            (saved.batteryConfigurationFlags & BATTERY_METADATA_CONFIGURED_FLAG) != 0U;
    }

    Configuration restored{
        BatterySensorConfiguration{
            ina219Configured,
            batteryMetadataConfigured,
            saved.shuntResistanceOhms,
            saved.maximumExpectedCurrentAmps,
            saved.emaAlpha,
            saved.batteryCapacityAmpHours,
            saved.initialStateOfChargePercent,
            saved.nominalVoltageVolts},
        PowerPlanningConfiguration{
            saved.powerPlanningConfigured != 0U,
            saved.P_budget,
            saved.P_reserve,
            saved.minimumStateOfChargePercent,
            saved.requiredRuntimeHours}};

    if (!isValidBatterySensor(restored.batterySensor) ||
        !isValidPowerPlanning(restored.powerPlanning)) {
        return false;
    }

    configuration_ = restored;
    return true;
#else
    return false;
#endif
}

bool CentralConfigurationStore::persist() const
{
#ifdef ESP_PLATFORM
    if (!isValidBatterySensor(configuration_.batterySensor) ||
        !isValidPowerPlanning(configuration_.powerPlanning)) return false;

    PersistedConfiguration saved{};
    const auto& battery = configuration_.batterySensor;
    const auto& planning = configuration_.powerPlanning;

    saved.batteryConfigurationFlags = 0U;
    if (battery.ina219Configured) {
        saved.batteryConfigurationFlags |= INA219_CONFIGURED_FLAG;
    }
    if (battery.batteryMetadataConfigured) {
        saved.batteryConfigurationFlags |= BATTERY_METADATA_CONFIGURED_FLAG;
    }
    saved.shuntResistanceOhms = battery.shuntResistanceOhms;
    saved.maximumExpectedCurrentAmps = battery.maximumExpectedCurrentAmps;
    saved.emaAlpha = battery.emaAlpha;
    saved.batteryCapacityAmpHours = battery.batteryCapacityAmpHours;
    saved.initialStateOfChargePercent = battery.initialStateOfChargePercent;
    saved.nominalVoltageVolts = battery.nominalVoltageVolts;

    saved.powerPlanningConfigured = planning.configured ? 1U : 0U;
    saved.P_budget = planning.P_budget;
    saved.P_reserve = planning.P_reserve;
    saved.minimumStateOfChargePercent = planning.minimumStateOfChargePercent;
    saved.requiredRuntimeHours = planning.requiredRuntimeHours;

    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_OK) result = nvs_set_u8(handle, VERSION_KEY, SAVED_VERSION);
    if (result == ESP_OK) result = nvs_set_blob(handle, DATA_KEY, &saved, sizeof(saved));
    if (result == ESP_OK) result = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    return result == ESP_OK;
#else
    return false;
#endif
}

} // namespace kilowatts
