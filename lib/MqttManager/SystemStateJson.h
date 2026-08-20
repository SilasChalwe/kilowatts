/**
 * @file SystemStateJson.h
 * @brief Declares JSON construction for the kilowatts/v1/state/system
 *        MQTT topic.
 *
 * SystemStateJson's one responsibility: format the already-computed
 * system-wide values the mobile application needs into the kilowatts/v1
 * JSON contract's system-state payload. It does not calculate any of
 * these values itself — BatteryStateOfCharge, SafePowerLimitCalculator,
 * AvailablePowerManager, BestFirstSearch, WiFiManager, MqttManager and
 * CurrentTimeProvider all supply their own already-computed results
 * through the plain SystemStateInputs struct below, so this class has
 * zero business logic of its own and is pure, hardware-free string
 * formatting.
 */

#ifndef KILOWATTS_SYSTEM_STATE_JSON_H
#define KILOWATTS_SYSTEM_STATE_JSON_H

#include <cstdint>
#include <string>

namespace kilowatts {


/**
 * Every value the kilowatts/v1/state/system payload needs, already
 * computed by the module that owns it. Nothing here is measured or
 * calculated by SystemStateJson itself.
 */
struct SystemStateInputs {
    /** False when no battery sensor has been commissioned — battery* fields below are then not meaningful and are published as 0/invalid rather than fabricated. */
    bool batterySensorConfigured;
    float batteryVoltageVolts;
    float batteryCurrentAmps;
    /** "NONE" | "HARDWARE" — see INA219Monitor::MeasurementSource. Always "NONE" when !batterySensorConfigured. */
    const char* batteryMeasurementSourceText;

    float stateOfChargePercent;
    /** See BatteryStateOfCharge::isValid() — never presented as a genuine reading when false. */
    bool stateOfChargeValid;
    /** "UNKNOWN" | "PERSISTED" | "INITIAL_COMMISSIONING" | "COULOMB_COUNTING" — see BatteryStateOfCharge::StateOfChargeSource. */
    const char* stateOfChargeSourceText;

    /** Conservative estimate derived from relay confirmations and ratings, not a per-load sensor total. */
    float estimatedTotalLoadPowerWatts;
    float availablePowerWatts;                 // P_available
    float fixedOnRunningPowerWatts;             // P_fixed
    float powerAvailableForAutoLoadsWatts;      // P_remaining at cycle start
    float remainingPowerWatts;                  // P_remaining after Best-First Search
    float committedPowerWatts;                  // P_committed after Best-First Search

    bool wifiConnected;
    const char* wifiStateText;
    bool mqttConnected;

    bool currentTimeValid;
    const char* currentTimeSourceText;          // "NONE" | "NTP" | "MANUAL"
    std::int64_t lastOptimizationEpochSeconds;   // 0 when no cycle has run yet

    std::uint32_t faultCount;
    const char* faultSummaryText;                // "" when faultCount == 0
};


class SystemStateJson {

public:

    /**
     * Builds the complete kilowatts/v1/state/system JSON payload,
     * including "schemaVersion".
     */
    static std::string build(const SystemStateInputs& inputs, std::uint32_t schemaVersion);
};


} // namespace kilowatts

#endif // KILOWATTS_SYSTEM_STATE_JSON_H
