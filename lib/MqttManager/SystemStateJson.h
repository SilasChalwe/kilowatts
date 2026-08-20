/**
 * @file SystemStateJson.h
 * @brief Dashboard system-state payload.
 */
#ifndef KILOWATTS_SYSTEM_STATE_JSON_H
#define KILOWATTS_SYSTEM_STATE_JSON_H

#include <cstdint>
#include <string>

namespace kilowatts {

struct SystemStateInputs {
    bool batterySensorConfigured;
    float batteryVoltageVolts;
    float batteryCurrentAmps;
    float batteryPowerWatts;
    const char* batteryMeasurementSourceText;

    float stateOfChargePercent;
    bool stateOfChargeValid;
    const char* stateOfChargeSourceText;

    float estimatedRuntimeHours;
    bool runtimeEstimateValid;

    /** Conservative estimate from confirmed relay states and configured load ratings. */
    float estimatedCurrentlyOnPowerWatts;

    /** Safe power the system may use this planning cycle. */
    float safeAvailablePowerWatts;

    /** Power required by Fixed-ON loads after protection has set their targets. */
    float fixedOnLoadPowerWatts;

    /** safeAvailablePowerWatts - fixedOnLoadPowerWatts. */
    float initialBestFirstPowerWatts;

    /** Power of Auto loads selected by Best-First Search. */
    float selectedAutoLoadPowerWatts;

    /** Power still unused after Best-First Search. */
    float finalRemainingPowerWatts;

    bool wifiConnected;
    const char* wifiStateText;
    bool mqttConnected;

    bool currentTimeValid;
    const char* currentTimeSourceText;
    std::int64_t lastOptimizationEpochSeconds;

    std::uint32_t faultCount;
    const char* faultSummaryText;
};

class SystemStateJson {
public:
    static std::string build(const SystemStateInputs& inputs, std::uint32_t schemaVersion);
};

} // namespace kilowatts

#endif // KILOWATTS_SYSTEM_STATE_JSON_H
