#include "PowerManager.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#endif

namespace kilowatts {


// ---------------------------------------------------------------------------
// BatteryStateOfCharge
// ---------------------------------------------------------------------------

#ifdef ESP_PLATFORM
static const char *SOC_TAG = "BATTERY_SOC";

namespace {
constexpr const char* SOC_NVS_NAMESPACE = "kw_battery";
constexpr const char* SOC_NVS_KEY_STATE_OF_CHARGE = "soc";
} // namespace
#endif


const char* toText(StateOfChargeSource source)
{
    switch (source) {
        case StateOfChargeSource::UNKNOWN: return "UNKNOWN";
        case StateOfChargeSource::PERSISTED: return "PERSISTED";
        case StateOfChargeSource::INITIAL_COMMISSIONING: return "INITIAL_COMMISSIONING";
        case StateOfChargeSource::COULOMB_COUNTING: return "COULOMB_COUNTING";
    }

    return "UNKNOWN";
}


BatteryStateOfCharge::BatteryStateOfCharge()
    : initialized_(false),
      batteryCapacityAmpHours_(0.0F),
      stateOfChargePercent_(0.0F),
      source_(StateOfChargeSource::UNKNOWN)
{
}


float BatteryStateOfCharge::clampPercent(float value)
{
    return std::max(0.0F, std::min(value, 100.0F));
}


bool BatteryStateOfCharge::initialize(float batteryCapacityAmpHours, float defaultStateOfChargePercent)
{
    if (!std::isfinite(batteryCapacityAmpHours) || batteryCapacityAmpHours <= 0.0F ||
        !std::isfinite(defaultStateOfChargePercent) ||
        defaultStateOfChargePercent < 0.0F || defaultStateOfChargePercent > 100.0F) {
#ifdef ESP_PLATFORM
        ESP_LOGE(SOC_TAG, "initialize() rejected: capacity=%.3fAh defaultSoC=%.1f%%",
                 static_cast<double>(batteryCapacityAmpHours), static_cast<double>(defaultStateOfChargePercent));
#endif
        return false;
    }

    batteryCapacityAmpHours_ = batteryCapacityAmpHours;

    float persistedStateOfCharge = 0.0F;
    if (loadPersistedStateOfChargePercent(persistedStateOfCharge)) {
        stateOfChargePercent_ = persistedStateOfCharge;
        source_ = StateOfChargeSource::PERSISTED;
#ifdef ESP_PLATFORM
        ESP_LOGI(SOC_TAG, "BATTERY_SOC: valid persisted record found; SoC=%.2f%% source=PERSISTED capacity=%.3fAh",
                 static_cast<double>(stateOfChargePercent_), static_cast<double>(batteryCapacityAmpHours_));
#endif
    } else {
        stateOfChargePercent_ = defaultStateOfChargePercent;
        source_ = StateOfChargeSource::INITIAL_COMMISSIONING;
#ifdef ESP_PLATFORM
        ESP_LOGI(SOC_TAG, "BATTERY_SOC: No valid persisted SoC estimate; SoC=%.2f%% source=INITIAL_COMMISSIONING capacity=%.3fAh",
                 static_cast<double>(stateOfChargePercent_), static_cast<double>(batteryCapacityAmpHours_));
#endif
    }

    initialized_ = true;
    return true;
}


bool BatteryStateOfCharge::isInitialized() const
{
    return initialized_;
}


bool BatteryStateOfCharge::isValid() const
{
    return source_ != StateOfChargeSource::UNKNOWN;
}


StateOfChargeSource BatteryStateOfCharge::getSource() const
{
    return source_;
}


bool BatteryStateOfCharge::update(float batteryCurrentAmps, float deltaTimeSeconds)
{
    if (!initialized_ ||
        !std::isfinite(batteryCurrentAmps) ||
        !std::isfinite(deltaTimeSeconds) || deltaTimeSeconds <= 0.0F) {
        return false;
    }

    /*
     * Discharge current is positive, so a positive I_B reduces SoC and a
     * negative (charging) I_B increases it.
     */
    const float dischargedPercent =
        (100.0F * batteryCurrentAmps * deltaTimeSeconds) / (3600.0F * batteryCapacityAmpHours_);

    stateOfChargePercent_ = clampPercent(stateOfChargePercent_ - dischargedPercent);
    source_ = StateOfChargeSource::COULOMB_COUNTING;

    return true;
}


float BatteryStateOfCharge::getStateOfChargePercent() const
{
    return stateOfChargePercent_;
}


bool BatteryStateOfCharge::persist() const
{
    if (!initialized_) {
        return false;
    }

    return persistStateOfChargePercent(stateOfChargePercent_);
}


bool BatteryStateOfCharge::loadPersistedStateOfChargePercent(float& stateOfChargePercent) const
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(SOC_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (result != ESP_OK) {
        return false;
    }

    std::uint32_t rawBits = 0U;
    result = nvs_get_u32(handle, SOC_NVS_KEY_STATE_OF_CHARGE, &rawBits);
    nvs_close(handle);

    if (result != ESP_OK) {
        return false;
    }

    float restored = 0.0F;
    static_assert(sizeof(rawBits) == sizeof(restored), "float must be 32 bits to round-trip through NVS u32 storage");
    std::memcpy(&restored, &rawBits, sizeof(restored));

    if (!std::isfinite(restored) || restored < 0.0F || restored > 100.0F) {
        ESP_LOGW(SOC_TAG, "Discarding corrupt persisted SoC value");
        return false;
    }

    stateOfChargePercent = restored;
    return true;
#else
    (void)stateOfChargePercent;
    return false;
#endif
}


bool BatteryStateOfCharge::persistStateOfChargePercent(float stateOfChargePercent) const
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    esp_err_t result = nvs_open(SOC_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        ESP_LOGW(SOC_TAG, "Could not open NVS to persist SoC: %s", esp_err_to_name(result));
        return false;
    }

    std::uint32_t rawBits = 0U;
    static_assert(sizeof(rawBits) == sizeof(stateOfChargePercent), "float must be 32 bits to round-trip through NVS u32 storage");
    std::memcpy(&rawBits, &stateOfChargePercent, sizeof(stateOfChargePercent));

    result = nvs_set_u32(handle, SOC_NVS_KEY_STATE_OF_CHARGE, rawBits);
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }

    nvs_close(handle);
    return result == ESP_OK;
#else
    (void)stateOfChargePercent;
    return false;
#endif
}


// ---------------------------------------------------------------------------
// SafePowerLimitCalculator
// ---------------------------------------------------------------------------

#ifdef ESP_PLATFORM
static const char *POWER_LIMIT_TAG = "SAFE_POWER_LIMIT_CALCULATOR";
#endif


SafePowerLimitCalculator::SafePowerLimitCalculator()
    : hasResult_(false),
      ratedEnergyWattHours_(0.0F),
      usableEnergyWattHours_(0.0F),
      runtimePowerWatts_(0.0F),
      maximumBatteryPowerWatts_(0.0F),
      maximumMainPowerWatts_(0.0F),
      availablePowerWatts_(0.0F)
{
}


bool SafePowerLimitCalculator::isFinitePercent(float value)
{
    return std::isfinite(value) && value >= 0.0F && value <= 100.0F;
}


bool SafePowerLimitCalculator::isFinitePositive(float value)
{
    return std::isfinite(value) && value > 0.0F;
}


bool SafePowerLimitCalculator::isFiniteNonNegative(float value)
{
    return std::isfinite(value) && value >= 0.0F;
}


bool SafePowerLimitCalculator::calculate(const Inputs& inputs)
{
    if (!isFinitePercent(inputs.stateOfChargePercent) ||
        !isFinitePercent(inputs.minimumStateOfChargePercent) ||
        !isFinitePositive(inputs.nominalBatteryVoltageVolts) ||
        !isFinitePositive(inputs.batteryCapacityAmpHours) ||
        !isFinitePositive(inputs.targetRuntimeHours) ||
        !isFinitePositive(inputs.batteryBusVoltageVolts) ||
        !isFiniteNonNegative(inputs.maximumBatteryDischargeCurrentAmps) ||
        !isFiniteNonNegative(inputs.maximumMainCurrentAmps) ||
        !std::isfinite(inputs.safetyFactor) ||
        inputs.safetyFactor <= 0.0F || inputs.safetyFactor > 1.0F) {
#ifdef ESP_PLATFORM
        ESP_LOGW(POWER_LIMIT_TAG, "calculate() rejected: SoC=%.1f%% SoCmin=%.1f%% Vnom=%.2fV CB=%.2fAh Ttarget=%.2fh VB=%.2fV IBmax=%.2fA Imainmax=%.2fA rho=%.3f",
                 static_cast<double>(inputs.stateOfChargePercent),
                 static_cast<double>(inputs.minimumStateOfChargePercent),
                 static_cast<double>(inputs.nominalBatteryVoltageVolts),
                 static_cast<double>(inputs.batteryCapacityAmpHours),
                 static_cast<double>(inputs.targetRuntimeHours),
                 static_cast<double>(inputs.batteryBusVoltageVolts),
                 static_cast<double>(inputs.maximumBatteryDischargeCurrentAmps),
                 static_cast<double>(inputs.maximumMainCurrentAmps),
                 static_cast<double>(inputs.safetyFactor));
#endif
        return false;
    }

    const float ratedEnergyWattHours = inputs.nominalBatteryVoltageVolts * inputs.batteryCapacityAmpHours;

    const float usableEnergyWattHours =
        ratedEnergyWattHours * std::max(0.0F, (inputs.stateOfChargePercent - inputs.minimumStateOfChargePercent) / 100.0F);

    const float runtimePowerWatts = usableEnergyWattHours / inputs.targetRuntimeHours;

    const float maximumBatteryPowerWatts = inputs.batteryBusVoltageVolts * inputs.maximumBatteryDischargeCurrentAmps;

    const float maximumMainPowerWatts = inputs.batteryBusVoltageVolts * inputs.maximumMainCurrentAmps;

    const float availablePowerWatts =
        inputs.safetyFactor * std::min(runtimePowerWatts, std::min(maximumBatteryPowerWatts, maximumMainPowerWatts));

    if (!std::isfinite(ratedEnergyWattHours) || !std::isfinite(usableEnergyWattHours) ||
        !std::isfinite(runtimePowerWatts) || !std::isfinite(maximumBatteryPowerWatts) ||
        !std::isfinite(maximumMainPowerWatts) || !std::isfinite(availablePowerWatts)) {
#ifdef ESP_PLATFORM
        ESP_LOGE(POWER_LIMIT_TAG, "calculate() produced a non-finite result; rejecting");
#endif
        return false;
    }

    ratedEnergyWattHours_ = ratedEnergyWattHours;
    usableEnergyWattHours_ = usableEnergyWattHours;
    runtimePowerWatts_ = runtimePowerWatts;
    maximumBatteryPowerWatts_ = maximumBatteryPowerWatts;
    maximumMainPowerWatts_ = maximumMainPowerWatts;
    availablePowerWatts_ = availablePowerWatts;
    hasResult_ = true;

#ifdef ESP_PLATFORM
    ESP_LOGI(POWER_LIMIT_TAG, "Erated=%.2fWh Eusable=%.2fWh Pruntime=%.2fW Pbatmax=%.2fW Pmainmax=%.2fW Pavailable=%.2fW",
             static_cast<double>(ratedEnergyWattHours_),
             static_cast<double>(usableEnergyWattHours_),
             static_cast<double>(runtimePowerWatts_),
             static_cast<double>(maximumBatteryPowerWatts_),
             static_cast<double>(maximumMainPowerWatts_),
             static_cast<double>(availablePowerWatts_));
#endif

    return true;
}


bool SafePowerLimitCalculator::hasResult() const
{
    return hasResult_;
}


float SafePowerLimitCalculator::getRatedEnergyWattHours() const
{
    return ratedEnergyWattHours_;
}


float SafePowerLimitCalculator::getUsableEnergyWattHours() const
{
    return usableEnergyWattHours_;
}


float SafePowerLimitCalculator::getRuntimePowerWatts() const
{
    return runtimePowerWatts_;
}


float SafePowerLimitCalculator::getMaximumBatteryPowerWatts() const
{
    return maximumBatteryPowerWatts_;
}


float SafePowerLimitCalculator::getMaximumMainPowerWatts() const
{
    return maximumMainPowerWatts_;
}


float SafePowerLimitCalculator::getAvailablePowerWatts() const
{
    return availablePowerWatts_;
}


// ---------------------------------------------------------------------------
// AvailablePowerManager
// ---------------------------------------------------------------------------

namespace {

bool isValidTotalAvailablePower(float totalAvailablePowerWatts)
{
    return std::isfinite(totalAvailablePowerWatts) && totalAvailablePowerWatts >= 0.0F;
}


float calculateFixedOnRunningPowerWatts(const LoadFilter& loadFilter)
{
    float fixedOnRunningPowerWatts = 0.0F;

    for (std::size_t i = 0; i < loadFilter.getNumberOfFixedOnLoads(); ++i) {
        const Load* fixedOnLoad = loadFilter.getFixedOnLoad(i);
        if (fixedOnLoad != nullptr) {
            fixedOnRunningPowerWatts += fixedOnLoad->getPower().runningWatts;
        }
    }

    return fixedOnRunningPowerWatts;
}

} // namespace


AvailablePowerManager::AvailablePowerManager()
    : totalAvailablePowerWatts_(0.0F),
      fixedOnRunningPowerWatts_(0.0F),
      powerAvailableForAutoLoadsWatts_(0.0F)
{
}


bool AvailablePowerManager::calculateAvailablePower(
    float totalAvailablePowerWatts,
    const LoadFilter& loadFilter)
{
    if (!isValidTotalAvailablePower(totalAvailablePowerWatts)) {
        return false;
    }

    const float fixedOnRunningPowerWatts = calculateFixedOnRunningPowerWatts(loadFilter);

    float powerAvailableForAutoLoadsWatts = totalAvailablePowerWatts - fixedOnRunningPowerWatts;
    if (powerAvailableForAutoLoadsWatts < 0.0F) {
        powerAvailableForAutoLoadsWatts = 0.0F;
    }

    totalAvailablePowerWatts_ = totalAvailablePowerWatts;
    fixedOnRunningPowerWatts_ = fixedOnRunningPowerWatts;
    powerAvailableForAutoLoadsWatts_ = powerAvailableForAutoLoadsWatts;

    return true;
}


float AvailablePowerManager::getTotalAvailablePowerWatts() const
{
    return totalAvailablePowerWatts_;
}


float AvailablePowerManager::getFixedOnRunningPowerWatts() const
{
    return fixedOnRunningPowerWatts_;
}


float AvailablePowerManager::getPowerAvailableForAutoLoadsWatts() const
{
    return powerAvailableForAutoLoadsWatts_;
}


} // namespace kilowatts
