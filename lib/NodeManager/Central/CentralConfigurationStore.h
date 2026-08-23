/**
 * @file CentralConfigurationStore.h
 * @brief Persists Central battery-monitor configuration and electrical limits.
 *
 * CentralConfigurationStore stores configuration only.
 *
 * PowerManager is responsible for:
 * - reading battery voltage/current/power,
 * - maintaining battery State of Charge (SoC),
 * - calculating P_remaining and P_available.
 *
 * This store does not calculate power, select Loads, control relays,
 * or model charger state.
 */
#ifndef KILOWATTS_CENTRAL_CONFIGURATION_STORE_H
#define KILOWATTS_CENTRAL_CONFIGURATION_STORE_H

namespace kilowatts {

class CentralConfigurationStore {
public:

    /**
     * @brief Configuration required for the Central INA219 battery monitor
     *        and SoC tracking.
     */
    struct BatterySensorConfiguration {
        bool configured;

        float shuntResistanceOhms;
        float maximumExpectedCurrentAmps;
        float emaAlpha;

        float batteryCapacityAmpHours;
        float initialStateOfChargePercent;
        float nominalVoltageVolts;
    };


    /**
     * @brief Electrical limits and runtime target used by PowerManager
     *        when calculating P_available for automatic Loads.
     *
     * These are limits/targets only. They do not represent charger state
     * and they do not perform Load selection.
     *
     * minimumStateOfChargePercent serves double duty as both the
     * immediate battery-protection cutoff and the reserve SoC below
     * which no battery energy counts as usable for runtime planning.
     *
     * requiredRuntimeHours is the user's requested "the system/battery
     * should last at least this many hours" target. 0 means no runtime
     * target is configured — PowerManager then applies only the
     * immediate electrical limits below, exactly as before this field
     * existed.
     */
    struct PowerLimitsConfiguration {
        bool configured;

        float minimumStateOfChargePercent;
        float maximumBatteryDischargeCurrentAmps;
        float maximumMainCurrentAmps;
        float requiredRuntimeHours;
    };


    struct Configuration {
        BatterySensorConfiguration batterySensor;
        PowerLimitsConfiguration powerLimits;
    };


    CentralConfigurationStore();


    const Configuration& getConfiguration() const;


    bool setBatterySensor(
        const BatterySensorConfiguration& configuration);


    bool setPowerLimits(
        const PowerLimitsConfiguration& configuration);


    bool loadPersisted();

    bool persist() const;


    static bool isValidBatterySensor(
        const BatterySensorConfiguration& configuration);


    static bool isValidPowerLimits(
        const PowerLimitsConfiguration& configuration);


private:

    Configuration configuration_;
};

} // namespace kilowatts

#endif // KILOWATTS_CENTRAL_CONFIGURATION_STORE_H