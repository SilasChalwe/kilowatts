#ifndef KILOWATTS_SYSTEM_STATE_JSON_H
#define KILOWATTS_SYSTEM_STATE_JSON_H

#include <cstdint>
#include <string>

namespace kilowatts {

struct SystemStateInputs {
    bool batterySensorConfigured;
    float batteryNominalVoltageVolts;
    float batteryCapacityAmpHours;
    float batteryRatedEnergyWattHours;
    float batteryStoredEnergyWattHours;
    float batteryUsableEnergyWattHours;

    float batteryVoltageVolts;
    float batteryCurrentAmps;
    float measuredSourcePowerWatts;
    const char* batteryMeasurementSourceText;

    float stateOfChargePercent;
    bool stateOfChargeValid;
    const char* stateOfChargeSourceText;

    float targetRuntimeHours;
    float estimatedRuntimeHours;
    bool runtimeEstimateValid;
    float runtimePowerLimitWatts;

    float maximumBatteryCurrentAmps;
    float maximumBatteryPowerWatts;
    float maximumMainCurrentAmps;
    float maximumMainPowerWatts;
    float safetyCeilingWatts;

    float powerBeforeFixedLoadsWatts;
    float fixedOnLoadPowerWatts;
    float powerPassedToBestFirstWatts;
    float selectedAutoLoadPowerWatts;
    float finalRemainingPowerWatts;

    bool wifiConnected;
    const char* wifiStateText;
    bool mqttConnected;

    bool currentTimeValid;
    const char* currentTimeSourceText;
    std::int64_t lastOptimizationEpochSeconds;

    std::uint32_t pinCommandErrorCount;
};

class SystemStateJson {
public:
    static std::string build(const SystemStateInputs& inputs, std::uint32_t schemaVersion);
};

} // namespace kilowatts

#endif // KILOWATTS_SYSTEM_STATE_JSON_H
