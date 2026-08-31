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
    float P_measured;
    const char* batteryMeasurementSourceText;

    float stateOfChargePercent;
    bool stateOfChargeValid;
    const char* stateOfChargeSourceText;
    bool batteryReserveReached;

    bool requiredRuntimeConfigured;
    float requiredRuntimeHours;
    float remainingRuntimeHours;
    float estimatedRuntimeHours;
    bool runtimeEstimateValid;
    bool requiredRuntimeAchievable;

    float P_budget;
    float P_reserve;
    float P_fixed;
    float P_auto_available;
    float P_auto;
    float P_remaining;

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
