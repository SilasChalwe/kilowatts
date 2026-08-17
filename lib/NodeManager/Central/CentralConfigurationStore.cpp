#include "CentralConfigurationStore.h"

#include <cstddef>
#include <cmath>

#ifdef ESP_PLATFORM
#include "nvs.h"
#endif

namespace kilowatts {

namespace {

constexpr const char* NVS_NAMESPACE = "kw_central_cfg";
constexpr const char* NVS_KEY_SCHEMA = "schema";
constexpr const char* NVS_KEY_CONFIGURATION = "config";
/* Version 2 adds batteryNominalVoltageVolts; a differently-sized Version 1 blob is safely rejected by loadPersisted()'s own size check, not misread. */
constexpr std::uint8_t SCHEMA_VERSION = 2U;

#pragma pack(push, 1)
struct PersistedConfiguration {
    std::uint8_t batteryConfigured;
    std::uint8_t batteryI2cAddress;
    float batteryShuntResistanceOhms;
    float batteryMaximumExpectedCurrentAmps;
    float batteryEmaAlpha;
    float batteryCapacityAmpHours;
    float batteryInitialStateOfChargePercent;
    float batteryNominalVoltageVolts;
    std::uint8_t safetyConfigured;
    float minimumStateOfChargePercent;
    float warningStateOfChargePercent;
    float targetRuntimeHours;
    float safetyFactor;
    float maximumBatteryDischargeCurrentAmps;
    float maximumMainCurrentAmps;
};
#pragma pack(pop)

CentralConfigurationStore::Configuration unconfigured()
{
    return CentralConfigurationStore::Configuration{
        CentralConfigurationStore::BatterySensorConfiguration{false, 0U, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F},
        CentralConfigurationStore::SafetyPolicy{false, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F},
    };
}

} // namespace

CentralConfigurationStore::CentralConfigurationStore() : configuration_(unconfigured()) {}

const CentralConfigurationStore::Configuration& CentralConfigurationStore::getConfiguration() const
{
    return configuration_;
}

bool CentralConfigurationStore::isValidBatterySensor(const BatterySensorConfiguration& configuration)
{
    if (!configuration.configured) {
        return true;
    }
    return configuration.i2cAddress >= 0x40U && configuration.i2cAddress <= 0x4FU &&
           std::isfinite(configuration.shuntResistanceOhms) && configuration.shuntResistanceOhms > 0.0F &&
           std::isfinite(configuration.maximumExpectedCurrentAmps) && configuration.maximumExpectedCurrentAmps > 0.0F &&
           std::isfinite(configuration.emaAlpha) && configuration.emaAlpha > 0.0F && configuration.emaAlpha <= 1.0F &&
           std::isfinite(configuration.batteryCapacityAmpHours) && configuration.batteryCapacityAmpHours > 0.0F &&
           std::isfinite(configuration.initialStateOfChargePercent) &&
           configuration.initialStateOfChargePercent >= 0.0F && configuration.initialStateOfChargePercent <= 100.0F &&
           std::isfinite(configuration.nominalVoltageVolts) && configuration.nominalVoltageVolts > 0.0F;
}

bool CentralConfigurationStore::isValidSafetyPolicy(const SafetyPolicy& policy)
{
    if (!policy.configured) {
        return true;
    }
    return std::isfinite(policy.minimumStateOfChargePercent) &&
           std::isfinite(policy.warningStateOfChargePercent) &&
           policy.minimumStateOfChargePercent >= 0.0F && policy.warningStateOfChargePercent <= 100.0F &&
           policy.warningStateOfChargePercent >= policy.minimumStateOfChargePercent &&
           std::isfinite(policy.targetRuntimeHours) && policy.targetRuntimeHours > 0.0F &&
           std::isfinite(policy.safetyFactor) && policy.safetyFactor > 0.0F && policy.safetyFactor <= 1.0F &&
           std::isfinite(policy.maximumBatteryDischargeCurrentAmps) && policy.maximumBatteryDischargeCurrentAmps > 0.0F &&
           std::isfinite(policy.maximumMainCurrentAmps) && policy.maximumMainCurrentAmps > 0.0F;
}

bool CentralConfigurationStore::setBatterySensor(const BatterySensorConfiguration& configuration)
{
    if (!isValidBatterySensor(configuration)) {
        return false;
    }
    configuration_.batterySensor = configuration;
    return true;
}

bool CentralConfigurationStore::setSafetyPolicy(const SafetyPolicy& policy)
{
    if (!isValidSafetyPolicy(policy)) {
        return false;
    }
    configuration_.safetyPolicy = policy;
    return true;
}

bool CentralConfigurationStore::loadPersisted()
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    std::uint8_t schema = 0U;
    std::size_t size = sizeof(PersistedConfiguration);
    PersistedConfiguration persisted{};
    esp_err_t result = nvs_get_u8(handle, NVS_KEY_SCHEMA, &schema);
    if (result == ESP_OK && schema == SCHEMA_VERSION) {
        result = nvs_get_blob(handle, NVS_KEY_CONFIGURATION, &persisted, &size);
    }
    nvs_close(handle);
    if (result != ESP_OK || size != sizeof(PersistedConfiguration)) {
        return false;
    }

    const Configuration restored{
        BatterySensorConfiguration{
            persisted.batteryConfigured != 0U,
            persisted.batteryI2cAddress,
            persisted.batteryShuntResistanceOhms,
            persisted.batteryMaximumExpectedCurrentAmps,
            persisted.batteryEmaAlpha,
            persisted.batteryCapacityAmpHours,
            persisted.batteryInitialStateOfChargePercent,
            persisted.batteryNominalVoltageVolts,
        },
        SafetyPolicy{
            persisted.safetyConfigured != 0U,
            persisted.minimumStateOfChargePercent,
            persisted.warningStateOfChargePercent,
            persisted.targetRuntimeHours,
            persisted.safetyFactor,
            persisted.maximumBatteryDischargeCurrentAmps,
            persisted.maximumMainCurrentAmps,
        },
    };
    if (!isValidBatterySensor(restored.batterySensor) || !isValidSafetyPolicy(restored.safetyPolicy)) {
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
    PersistedConfiguration persisted{};
    persisted.batteryConfigured = configuration_.batterySensor.configured ? 1U : 0U;
    persisted.batteryI2cAddress = configuration_.batterySensor.i2cAddress;
    persisted.batteryShuntResistanceOhms = configuration_.batterySensor.shuntResistanceOhms;
    persisted.batteryMaximumExpectedCurrentAmps = configuration_.batterySensor.maximumExpectedCurrentAmps;
    persisted.batteryEmaAlpha = configuration_.batterySensor.emaAlpha;
    persisted.batteryCapacityAmpHours = configuration_.batterySensor.batteryCapacityAmpHours;
    persisted.batteryInitialStateOfChargePercent = configuration_.batterySensor.initialStateOfChargePercent;
    persisted.batteryNominalVoltageVolts = configuration_.batterySensor.nominalVoltageVolts;
    persisted.safetyConfigured = configuration_.safetyPolicy.configured ? 1U : 0U;
    persisted.minimumStateOfChargePercent = configuration_.safetyPolicy.minimumStateOfChargePercent;
    persisted.warningStateOfChargePercent = configuration_.safetyPolicy.warningStateOfChargePercent;
    persisted.targetRuntimeHours = configuration_.safetyPolicy.targetRuntimeHours;
    persisted.safetyFactor = configuration_.safetyPolicy.safetyFactor;
    persisted.maximumBatteryDischargeCurrentAmps = configuration_.safetyPolicy.maximumBatteryDischargeCurrentAmps;
    persisted.maximumMainCurrentAmps = configuration_.safetyPolicy.maximumMainCurrentAmps;

    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_u8(handle, NVS_KEY_SCHEMA, SCHEMA_VERSION);
    }
    if (result == ESP_OK) {
        result = nvs_set_blob(handle, NVS_KEY_CONFIGURATION, &persisted, sizeof(persisted));
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    if (handle != 0) {
        nvs_close(handle);
    }
    return result == ESP_OK;
#else
    return false;
#endif
}

} // namespace kilowatts
