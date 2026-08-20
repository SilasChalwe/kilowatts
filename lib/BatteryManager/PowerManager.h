/**
 * @file PowerManager.h
 * @brief Battery state-of-charge and safe system power calculations.
 */
#ifndef KILOWATTS_POWER_MANAGER_H
#define KILOWATTS_POWER_MANAGER_H

#include <cstdint>

namespace kilowatts {

enum class StateOfChargeSource : std::uint8_t {
    UNKNOWN = 0U,
    PERSISTED = 1U,
    INITIAL_COMMISSIONING = 2U,
    COULOMB_COUNTING = 3U
};

const char* toText(StateOfChargeSource source);

class BatteryStateOfCharge {
public:
    BatteryStateOfCharge();

    bool initialize(float batteryCapacityAmpHours, float startingStateOfChargePercent);
    bool update(float batteryCurrentAmps, float deltaTimeSeconds);

    bool isInitialized() const;
    bool isValid() const;
    float getStateOfChargePercent() const;
    StateOfChargeSource getSource() const;

    bool persist() const;

private:
    bool loadPersistedStateOfChargePercent(float& stateOfChargePercent) const;
    bool persistStateOfChargePercent(float stateOfChargePercent) const;
    static float clampPercent(float value);

    bool initialized_;
    float batteryCapacityAmpHours_;
    float stateOfChargePercent_;
    StateOfChargeSource source_;
};

class SafePowerLimitCalculator {
public:
    struct Inputs {
        float stateOfChargePercent;
        float minimumStateOfChargePercent;
        float nominalBatteryVoltageVolts;
        float batteryCapacityAmpHours;
        float targetRuntimeHours;
        float batteryBusVoltageVolts;
        float maximumBatteryDischargeCurrentAmps;
        float maximumMainCurrentAmps;
        float safetyFactor;
    };

    SafePowerLimitCalculator();

    bool calculate(const Inputs& inputs);
    bool hasResult() const;

    float getRatedEnergyWattHours() const;
    float getUsableEnergyWattHours() const;
    float getRuntimePowerWatts() const;
    float getMaximumBatteryPowerWatts() const;
    float getMaximumMainPowerWatts() const;
    float getAvailablePowerWatts() const;

private:
    static bool isFinitePercent(float value);
    static bool isFinitePositive(float value);
    static bool isFiniteNonNegative(float value);

    bool hasResult_;
    float ratedEnergyWattHours_;
    float usableEnergyWattHours_;
    float runtimePowerWatts_;
    float maximumBatteryPowerWatts_;
    float maximumMainPowerWatts_;
    float availablePowerWatts_;
};

} // namespace kilowatts

#endif // KILOWATTS_POWER_MANAGER_H
