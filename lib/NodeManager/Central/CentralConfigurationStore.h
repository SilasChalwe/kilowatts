/**
 * @file CentralConfigurationStore.h
 * @brief Persists battery and safety settings owned by Central.
 */
#ifndef KILOWATTS_CENTRAL_CONFIGURATION_STORE_H
#define KILOWATTS_CENTRAL_CONFIGURATION_STORE_H

#include <cstdint>

namespace kilowatts {

class CentralConfigurationStore {
public:
    struct BatterySensorConfiguration {
        bool configured;
        float shuntResistanceOhms;
        float maximumExpectedCurrentAmps;
        float emaAlpha;
        float batteryCapacityAmpHours;
        float initialStateOfChargePercent;
        float nominalVoltageVolts;
    };

    struct SafetyPolicy {
        bool configured;
        float minimumStateOfChargePercent;
        float warningStateOfChargePercent;
        float targetRuntimeHours;
        float safetyFactor;
        float maximumBatteryDischargeCurrentAmps;
        float maximumMainCurrentAmps;
    };

    struct Configuration {
        BatterySensorConfiguration batterySensor;
        SafetyPolicy safetyPolicy;
    };

    CentralConfigurationStore();

    const Configuration& getConfiguration() const;
    bool setBatterySensor(const BatterySensorConfiguration& configuration);
    bool setSafetyPolicy(const SafetyPolicy& policy);

    bool loadPersisted();
    bool persist() const;

    static bool isValidBatterySensor(const BatterySensorConfiguration& configuration);
    static bool isValidSafetyPolicy(const SafetyPolicy& policy);

private:
    Configuration configuration_;
};

} // namespace kilowatts

#endif // KILOWATTS_CENTRAL_CONFIGURATION_STORE_H
