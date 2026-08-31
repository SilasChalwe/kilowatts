/**
 * @file CentralConfigurationStore.h
 * @brief Persists Central INA219, battery, and power-planning configuration.
 */
#ifndef KILOWATTS_CENTRAL_CONFIGURATION_STORE_H
#define KILOWATTS_CENTRAL_CONFIGURATION_STORE_H

namespace kilowatts {

class CentralConfigurationStore {
public:
    struct BatterySensorConfiguration {
        bool ina219Configured;
        bool batteryMetadataConfigured;
        float shuntResistanceOhms;
        float maximumExpectedCurrentAmps;
        float emaAlpha;
        float batteryCapacityAmpHours;
        float initialStateOfChargePercent;
        float nominalVoltageVolts;
    };

    /**
     * Configuration used for load allocation and runtime planning.
     * No field in this structure represents hardware electrical protection.
     */
    struct PowerPlanningConfiguration {
        bool configured;
        float P_budget;
        float P_reserve;
        float minimumStateOfChargePercent;
        float requiredRuntimeHours;
    };

    struct Configuration {
        BatterySensorConfiguration batterySensor;
        PowerPlanningConfiguration powerPlanning;
    };

    CentralConfigurationStore();

    const Configuration& getConfiguration() const;
    bool setBatterySensor(const BatterySensorConfiguration& configuration);
    bool setPowerPlanning(const PowerPlanningConfiguration& configuration);
    bool loadPersisted();
    bool persist() const;

    static bool isValidBatterySensor(const BatterySensorConfiguration& configuration);
    static bool isValidPowerPlanning(const PowerPlanningConfiguration& configuration);

private:
    Configuration configuration_;
};

} // namespace kilowatts

#endif // KILOWATTS_CENTRAL_CONFIGURATION_STORE_H
