#include "CentralConfigurationStore.h"

#include <cmath>
#include <cstddef>

#ifdef ESP_PLATFORM
#include "nvs.h"
#endif

namespace kilowatts {

namespace {

constexpr const char* NVS_NAMESPACE = "kw_central_cfg";
constexpr const char* NVS_KEY_SCHEMA = "schema";
constexpr const char* NVS_KEY_CONFIGURATION = "config";
constexpr std::uint8_t SCHEMA_VERSION = 3U;

#pragma pack(push, 1)
struct PersistedConfiguration {
    std::uint8_t batteryConfigured;
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

CentralConfigurationStore::Configuration emptyConfiguration()
{
    return CentralConfigurationStore::Configuration{
        CentralConfigurationStore::BatterySensorConfiguration{
            false, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F},
        CentralConfigurationStore::SafetyPolicy{
            false, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F}};
}

} // namespace

CentralConfigurationStore::CentralConfigurationStore()
    : configuration_(emptyConfiguration())
{
}

const CentralConfigurationStore::Configuration& CentralConfigurationStore::getConfiguration() const
{
    return configuration_;
}

bool CentralConfigurationStore::isValidBatterySensor(const BatterySensorConfiguration& value)
{
    if (!value.configured) {
        return true;
    }

    return std::isfinite(value.shuntResistanceOhms) && value.shuntResistanceOhms > 0.0F &&
           std::isfinite(value.maximumExpectedCurrentAmps) && value.maximumExpectedCurrentAmps > 0.0F &&
           std::isfinite(value.emaAlpha) && value.emaAlpha > 0.0F && value.emaAlpha <= 1.0F &&
           std::isfinite(value.batteryCapacityAmpHours) && value.batteryCapacityAmpHours > 0.0F &&
           std::isfinite(value.initialStateOfChargePercent) &&
           value.initialStateOfChargePercent >= 0.0F && value.initialStateOfChargePercent <= 100.0F &&
           std::isfinite(value.nominalVoltageVolts) && value.nominalVoltageVolts > 0.0F;
}

bool CentralConfigurationStore::isValidSafetyPolicy(const SafetyPolicy& value)
{
    if (!value.configured) {
        return true;
    }

    return std::isfinite(value.minimumStateOfChargePercent) &&
           std::isfinite(value.warningStateOfChargePercent) &&
           value.minimumStateOfChargePercent >= 0.0F &&
           value.warningStateOfChargePercent <= 100.0F &&
           value.warningStateOfChargePercent >= value.minimumStateOfChargePercent &&
           std::isfinite(value.targetRuntimeHours) && value.targetRuntimeHours > 0.0F &&
           std::isfinite(value.safetyFactor) && value.safetyFactor > 0.0F && value.safetyFactor <= 1.0F &&
           std::isfinite(value.maximumBatteryDischargeCurrentAmps) &&
           value.maximumBatteryDischargeCurrentAmps > 0.0F &&
           std::isfinite(value.maximumMainCurrentAmps) && value.maximumMainCurrentAmps > 0.0F;
}

bool CentralConfigurationStore::setBatterySensor(const BatterySensorConfiguration& value)
{
    if (!isValidBatterySensor(value)) {
        return false;
    }
    configuration_.batterySensor = value;
    return true;
}

bool CentralConfigurationStore::setSafetyPolicy(const SafetyPolicy& value)
{
    if (!isValidSafetyPolicy(value)) {
        return false;
    }
    configuration_.safetyPolicy = value;
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
    PersistedConfiguration stored{};

    esp_err_t result = nvs_get_u8(handle, NVS_KEY_SCHEMA, &schema);
    if (result == ESP_OK && schema == SCHEMA_VERSION) {
        result = nvs_get_blob(handle, NVS_KEY_CONFIGURATION, &stored, &size);
    }
    nvs_close(handle);

    if (result != ESP_OK || size != sizeof(PersistedConfiguration)) {
        return false;
    }

    Configuration restored{
        BatterySensorConfiguration{
            stored.batteryConfigured != 0U,
            stored.batteryShuntResistanceOhms,
            stored.batteryMaximumExpectedCurrentAmps,
            stored.batteryEmaAlpha,
            stored.batteryCapacityAmpHours,
            stored.batteryInitialStateOfChargePercent,
            stored.batteryNominalVoltageVolts},
        SafetyPolicy{
            stored.safetyConfigured != 0U,
            stored.minimumStateOfChargePercent,
            stored.warningStateOfChargePercent,
            stored.targetRuntimeHours,
            stored.safetyFactor,
            stored.maximumBatteryDischargeCurrentAmps,
            stored.maximumMainCurrentAmps}};

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
    PersistedConfiguration stored{};
    stored.batteryConfigured = configuration_.batterySensor.configured ? 1U : 0U;
    stored.batteryShuntResistanceOhms = configuration_.batterySensor.shuntResistanceOhms;
    stored.batteryMaximumExpectedCurrentAmps = configuration_.batterySensor.maximumExpectedCurrentAmps;
    stored.batteryEmaAlpha = configuration_.batterySensor.emaAlpha;
    stored.batteryCapacityAmpHours = configuration_.batterySensor.batteryCapacityAmpHours;
    stored.batteryInitialStateOfChargePercent = configuration_.batterySensor.initialStateOfChargePercent;
    stored.batteryNominalVoltageVolts = configuration_.batterySensor.nominalVoltageVolts;
    stored.safetyConfigured = configuration_.safetyPolicy.configured ? 1U : 0U;
    stored.minimumStateOfChargePercent = configuration_.safetyPolicy.minimumStateOfChargePercent;
    stored.warningStateOfChargePercent = configuration_.safetyPolicy.warningStateOfChargePercent;
    stored.targetRuntimeHours = configuration_.safetyPolicy.targetRuntimeHours;
    stored.safetyFactor = configuration_.safetyPolicy.safetyFactor;
    stored.maximumBatteryDischargeCurrentAmps = configuration_.safetyPolicy.maximumBatteryDischargeCurrentAmps;
    stored.maximumMainCurrentAmps = configuration_.safetyPolicy.maximumMainCurrentAmps;

    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_u8(handle, NVS_KEY_SCHEMA, SCHEMA_VERSION);
    }
    if (result == ESP_OK) {
        result = nvs_set_blob(handle, NVS_KEY_CONFIGURATION, &stored, sizeof(stored));
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
