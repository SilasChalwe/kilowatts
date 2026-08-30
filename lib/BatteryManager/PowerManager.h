/**
 * @file PowerManager.h
 * @brief Central battery measurement and load-allocation budget management.
 *
 * The configured installation budget is the source of truth for planning.
 * INA219 or simulation supplies the same measurement inputs: voltage and
 * current. Their instantaneous power is monitoring data, not available power.
 */
#ifndef KILOWATTS_POWER_MANAGER_H
#define KILOWATTS_POWER_MANAGER_H

#include <cstdint>

namespace kilowatts {

struct PowerMeasurements {
    float voltageVolts;
    float currentAmps;
    float powerWatts;
};

enum class MeasurementSource : std::uint8_t {
    NONE = 0U,
    HARDWARE = 1U,
    SIMULATED = 2U
};

enum class StateOfChargeSource : std::uint8_t {
    UNKNOWN = 0U,
    INITIAL = 1U,
    PERSISTED = 2U,
    COULOMB_COUNTING = 3U,
    VOLTAGE_ESTIMATE = 4U,
    SIMULATED = 5U
};

/**
 * Canonical planning values.
 *
 * P_usable         = max(0, P_budget - P_reserve)
 * P_auto_available = max(0, planning allowance - P_fixed)
 * P_remaining      = max(0, P_budget - (P_fixed + P_auto))
 * P_measured       = measured voltage * measured current
 */
struct PowerBudget {
    float batteryVoltageVolts;
    float batteryCurrentAmps;
    float P_measured;

    float P_budget;
    float P_reserve;
    float P_usable;
    float P_fixed;
    float P_auto_available;
    float P_auto;
    float P_remaining;

    bool runtimeBudgetActive;
    float P_runtime;
    bool requiredRuntimeAchievable;
};

class PowerManager {
public:
    struct BusConfiguration {
        std::uint8_t sdaPin;
        std::uint8_t sclPin;
        std::uint8_t port;
        std::uint32_t clockSpeedHz;
    };

    /** maximumExpectedCurrentAmps is only an INA219 measurement-range input. */
    struct SensorConfiguration {
        float shuntResistanceOhms;
        float maximumExpectedCurrentAmps;
        float emaAlpha;
    };

    /**
     * Battery energy metadata used for SoC/runtime calculations.
     * minimumStateOfChargePercent is an energy reserve policy, not electrical
     * hardware protection.
     */
    struct BatteryConfiguration {
        float nameplateVoltageVolts;
        float capacityAmpHours;
        float minimumStateOfChargePercent;
    };

    struct Calibration {
        float voltageOffsetVolts;
        float currentOffsetAmps;
        float currentScaleFactor;
    };

    PowerManager();
    ~PowerManager();

    PowerManager(const PowerManager&) = delete;
    PowerManager& operator=(const PowerManager&) = delete;

    bool initialize(
        const BusConfiguration& busConfiguration,
        const SensorConfiguration& sensorConfiguration,
        const BatteryConfiguration& batteryConfiguration);

    bool isInitialized() const;

    bool updateMeasurements();
    PowerMeasurements getMeasurements() const;
    MeasurementSource getMeasurementSource() const;
    bool isHardwareSensorPresent() const;

    bool enableSimulation(bool enabled);
    bool isSimulationEnabled() const;
    bool setSimulatedMeasurements(float batteryVoltageVolts, float batteryCurrentAmps);
    bool setSimulatedStateOfChargePercent(float stateOfChargePercent);

    bool setCalibration(const Calibration& calibration);
    Calibration getCalibration() const;

    bool initializeStateOfCharge(float startingStateOfChargePercent, bool restorePersistedState = true);
    bool updateStateOfCharge(float deltaTimeSeconds);
    bool applyVoltageStateOfChargeEstimate(float emptyVoltageVolts, float fullVoltageVolts, float weight);
    float getStateOfChargePercent() const;
    StateOfChargeSource getStateOfChargeSource() const;
    bool isStateOfChargeValid() const;
    bool persistStateOfCharge() const;

    bool setPowerBudgetWatts(float P_budget);
    bool setPowerReserveWatts(float P_reserve);
    bool setFixedPowerWatts(float P_fixed);
    bool setAutoPowerWatts(float P_auto);
    bool setRemainingRequiredRuntimeHours(float remainingRequiredRuntimeHours);
    float getRemainingRequiredRuntimeHours() const;

    bool updatePowerBudget();
    PowerBudget getPowerBudget() const;
    float getAutoAvailablePowerWatts() const;
    float getRemainingPowerWatts() const;

    void printDiagnosticReport() const;

private:
    bool initializeBus();
    bool initializeSensor();
    bool readHardwareMeasurements(PowerMeasurements& measurements) const;
    bool readSimulatedMeasurements(PowerMeasurements& measurements) const;

    static PowerMeasurements applyCalibration(const PowerMeasurements& measurements, const Calibration& calibration);
    static PowerMeasurements applyExponentialMovingAverage(const PowerMeasurements& previous, const PowerMeasurements& current, float alpha);

    static bool isFinitePositive(float value);
    static bool isFiniteNonNegative(float value);
    static bool isValidPercent(float value);
    static bool isValidCalibration(const Calibration& calibration);

    bool loadPersistedStateOfCharge(float& stateOfChargePercent) const;
    bool persistStateOfChargeValue(float stateOfChargePercent) const;
    static float clampStateOfCharge(float value);

    BusConfiguration busConfiguration_;
    SensorConfiguration sensorConfiguration_;
    BatteryConfiguration batteryConfiguration_;
    Calibration calibration_;

    PowerMeasurements measurements_;
    PowerMeasurements filteredMeasurements_;
    PowerMeasurements simulatedMeasurements_;
    bool hasFilteredMeasurement_;
    MeasurementSource measurementSource_;

    float stateOfChargePercent_;
    StateOfChargeSource stateOfChargeSource_;
    bool stateOfChargeInitialized_;

    PowerBudget powerBudget_;
    float remainingRequiredRuntimeHours_;

    bool initialized_;
    bool simulationEnabled_;

    void* busHandle_;
    void* deviceHandle_;
};

const char* toText(MeasurementSource source);
const char* toText(StateOfChargeSource source);

} // namespace kilowatts

#endif // KILOWATTS_POWER_MANAGER_H
