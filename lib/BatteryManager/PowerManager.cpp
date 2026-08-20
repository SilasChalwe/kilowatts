#include "PowerManager.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#ifdef ESP_PLATFORM
#include "nvs.h"
#endif

namespace kilowatts {

namespace {
#ifdef ESP_PLATFORM
constexpr const char* SOC_NVS_NAMESPACE = "kw_battery";
constexpr const char* SOC_NVS_KEY = "soc";
#endif
}

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

bool BatteryStateOfCharge::initialize(float capacityAmpHours, float startingPercent)
{
    if (!std::isfinite(capacityAmpHours) || capacityAmpHours <= 0.0F ||
        !std::isfinite(startingPercent) || startingPercent < 0.0F || startingPercent > 100.0F) {
        return false;
    }

    batteryCapacityAmpHours_ = capacityAmpHours;

    float persistedPercent = 0.0F;
    if (loadPersistedStateOfChargePercent(persistedPercent)) {
        stateOfChargePercent_ = persistedPercent;
        source_ = StateOfChargeSource::PERSISTED;
    } else {
        stateOfChargePercent_ = startingPercent;
        source_ = StateOfChargeSource::INITIAL_COMMISSIONING;
    }

    initialized_ = true;
    return true;
}

bool BatteryStateOfCharge::update(float currentAmps, float seconds)
{
    if (!initialized_ || !std::isfinite(currentAmps) ||
        !std::isfinite(seconds) || seconds <= 0.0F) {
        return false;
    }

    const float percentUsed =
        (100.0F * currentAmps * seconds) / (3600.0F * batteryCapacityAmpHours_);

    stateOfChargePercent_ = clampPercent(stateOfChargePercent_ - percentUsed);
    source_ = StateOfChargeSource::COULOMB_COUNTING;
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

float BatteryStateOfCharge::getStateOfChargePercent() const
{
    return stateOfChargePercent_;
}

StateOfChargeSource BatteryStateOfCharge::getSource() const
{
    return source_;
}

bool BatteryStateOfCharge::persist() const
{
    return initialized_ && persistStateOfChargePercent(stateOfChargePercent_);
}

bool BatteryStateOfCharge::loadPersistedStateOfChargePercent(float& percent) const
{
#ifdef ESP_PLATFORM
    nvs_handle_t handle = 0;
    if (nvs_open(SOC_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    std::uint32_t bits = 0U;
    const esp_err_t result = nvs_get_u32(handle, SOC_NVS_KEY, &bits);
    nvs_close(handle);
    if (result != ESP_OK) {
        return false;
    }

    std::memcpy(&percent, &bits, sizeof(percent));
    return std::isfinite(percent) && percent >= 0.0F && percent <= 100.0F;
#else
    (void)percent;
    return false;
#endif
}

bool BatteryStateOfCharge::persistStateOfChargePercent(float percent) const
{
#ifdef ESP_PLATFORM
    std::uint32_t bits = 0U;
    std::memcpy(&bits, &percent, sizeof(percent));

    nvs_handle_t handle = 0;
    if (nvs_open(SOC_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    esp_err_t result = nvs_set_u32(handle, SOC_NVS_KEY, bits);
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result == ESP_OK;
#else
    (void)percent;
    return true;
#endif
}

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

bool SafePowerLimitCalculator::calculate(const Inputs& in)
{
    if (!isFinitePercent(in.stateOfChargePercent) ||
        !isFinitePercent(in.minimumStateOfChargePercent) ||
        !isFinitePositive(in.nominalBatteryVoltageVolts) ||
        !isFinitePositive(in.batteryCapacityAmpHours) ||
        !isFinitePositive(in.targetRuntimeHours) ||
        !isFinitePositive(in.batteryBusVoltageVolts) ||
        !isFiniteNonNegative(in.maximumBatteryDischargeCurrentAmps) ||
        !isFiniteNonNegative(in.maximumMainCurrentAmps) ||
        !std::isfinite(in.safetyFactor) || in.safetyFactor <= 0.0F || in.safetyFactor > 1.0F) {
        return false;
    }

    const float ratedEnergy = in.nominalBatteryVoltageVolts * in.batteryCapacityAmpHours;
    const float usableFraction = std::max(
        0.0F,
        (in.stateOfChargePercent - in.minimumStateOfChargePercent) / 100.0F);
    const float usableEnergy = ratedEnergy * usableFraction;
    const float runtimePower = usableEnergy / in.targetRuntimeHours;
    const float batteryLimit = in.batteryBusVoltageVolts * in.maximumBatteryDischargeCurrentAmps;
    const float mainLimit = in.batteryBusVoltageVolts * in.maximumMainCurrentAmps;
    const float availablePower = in.safetyFactor * std::min(runtimePower, std::min(batteryLimit, mainLimit));

    if (!std::isfinite(availablePower)) {
        return false;
    }

    ratedEnergyWattHours_ = ratedEnergy;
    usableEnergyWattHours_ = usableEnergy;
    runtimePowerWatts_ = runtimePower;
    maximumBatteryPowerWatts_ = batteryLimit;
    maximumMainPowerWatts_ = mainLimit;
    availablePowerWatts_ = availablePower;
    hasResult_ = true;
    return true;
}

bool SafePowerLimitCalculator::hasResult() const { return hasResult_; }
float SafePowerLimitCalculator::getRatedEnergyWattHours() const { return ratedEnergyWattHours_; }
float SafePowerLimitCalculator::getUsableEnergyWattHours() const { return usableEnergyWattHours_; }
float SafePowerLimitCalculator::getRuntimePowerWatts() const { return runtimePowerWatts_; }
float SafePowerLimitCalculator::getMaximumBatteryPowerWatts() const { return maximumBatteryPowerWatts_; }
float SafePowerLimitCalculator::getMaximumMainPowerWatts() const { return maximumMainPowerWatts_; }
float SafePowerLimitCalculator::getAvailablePowerWatts() const { return availablePowerWatts_; }

} // namespace kilowatts
