/**
 * @file PowerManager.h
 * @brief Central battery monitoring and power-budget management for Kilowatts.
 *
 * PowerManager is responsible for:
 *
 * 1. Reading the central battery-bus INA219.
 * 2. Maintaining battery State of Charge (SoC).
 * 3. Applying battery and main-bus electrical limits.
 * 4. Tracking power already committed to Fixed ON loads.
 * 5. Calculating P_remaining and P_available for automatic loads.
 *
 * PowerManager does NOT:
 *
 * - Rank loads.
 * - Select loads.
 * - Run Best-First Search.
 * - Control relays.
 * - Maintain Load objects.
 *
 * BestFirstSearch receives P_available from this component.
 */

#ifndef KILOWATTS_POWER_MANAGER_H
#define KILOWATTS_POWER_MANAGER_H

#include <cstdint>

namespace kilowatts {


/**
 * @brief Instantaneous measurements from the central battery bus.
 */
struct PowerMeasurements {
    float voltageVolts;
    float currentAmps;
    float powerWatts;
};


/**
 * @brief Source used for the latest electrical measurement.
 */
enum class MeasurementSource : std::uint8_t {
    NONE = 0U,
    HARDWARE = 1U,
    SIMULATED = 2U
};


/**
 * @brief Current battery State-of-Charge source.
 */
enum class StateOfChargeSource : std::uint8_t {
    UNKNOWN = 0U,
    INITIAL = 1U,
    PERSISTED = 2U,
    COULOMB_COUNTING = 3U,
    VOLTAGE_ESTIMATE = 4U,
    SIMULATED = 5U
};


/**
 * @brief Complete power budget used by the central controller.
 *
 * Terminology:
 *
 * P_battery_max = maximum battery power allowed.
 * P_committed   = power already committed to Fixed ON loads.
 * P_remaining   = safe system power remaining after committed power,
 *                 constrained only by the IMMEDIATE electrical limits
 *                 (battery/main current).
 * P_available   = power supplied to BestFirstSearch for Auto Loads —
 *                 the stricter of P_remaining and the runtime-based
 *                 sustainable budget below.
 *
 * Runtime sustainability terms (see setRemainingRequiredRuntimeHours()):
 * these are meaningful only while runtimeBudgetActive is true, i.e. a
 * required-runtime target is currently configured/applied. They do not
 * replace P_remaining's immediate electrical-safety role — both are
 * always considered, and the stricter one wins.
 */
struct PowerBudget {
    float batteryVoltageVolts;       // V_B
    float batteryCurrentAmps;        // I_B

    float batteryPowerWatts;

    float batteryMaximumPowerWatts;  // P_battery_max
    float mainMaximumPowerWatts;

    float committedPowerWatts;       // P_committed
    float remainingPowerWatts;       // P_remaining
    float availablePowerWatts;       // P_available

    /** True while a required-runtime target (> 0 h) is currently applied. */
    bool runtimeBudgetActive;

    /**
     * Total sustainable power (Fixed + Auto combined) the usable battery
     * energy can support for the remaining required runtime. Meaningful
     * only while runtimeBudgetActive is true.
     */
    float sustainableTotalPowerWatts;

    /**
     * False when P_committed (Fixed ON loads) alone already exceeds
     * sustainableTotalPowerWatts — the requested runtime cannot currently
     * be guaranteed even with zero Auto Loads. Fixed ON loads are never
     * switched off to chase this target; this is reporting only. True
     * whenever runtimeBudgetActive is false (no target configured).
     */
    bool requiredRuntimeAchievable;
};


class PowerManager {
public:

    /**
     * @brief Configuration of the central INA219 I2C bus.
     */
    struct BusConfiguration {
        std::uint8_t sdaPin;
        std::uint8_t sclPin;
        std::uint8_t port;
        std::uint32_t clockSpeedHz;
    };


    /**
     * @brief INA219 electrical configuration.
     */
    struct SensorConfiguration {
        float shuntResistanceOhms;
        float maximumExpectedCurrentAmps;
        float emaAlpha;
    };


    /**
     * @brief Battery configuration.
     */
    struct BatteryConfiguration {
        float nameplateVoltageVolts;
        float capacityAmpHours;

        float minimumStateOfChargePercent;

        /**
         * Maximum current the battery is allowed to supply.
         */
        float maximumDischargeCurrentAmps;
    };


    /**
     * @brief Main electrical distribution limit.
     */
    struct MainBusConfiguration {
        float maximumCurrentAmps;    // I_main_max
    };


    /**
     * @brief Electrical measurement calibration.
     */
    struct Calibration {
        float voltageOffsetVolts;
        float currentOffsetAmps;
        float currentScaleFactor;
    };


    PowerManager();
    ~PowerManager();

    PowerManager(const PowerManager&) = delete;
    PowerManager& operator=(const PowerManager&) = delete;


    // -----------------------------------------------------------------
    // Initialization
    // -----------------------------------------------------------------

    bool initialize(
        const BusConfiguration& busConfiguration,
        const SensorConfiguration& sensorConfiguration,
        const BatteryConfiguration& batteryConfiguration,
        const MainBusConfiguration& mainBusConfiguration);

    bool isInitialized() const;


    // -----------------------------------------------------------------
    // Measurement
    // -----------------------------------------------------------------

    bool updateMeasurements();

    PowerMeasurements getMeasurements() const;

    MeasurementSource getMeasurementSource() const;

    bool isHardwareSensorPresent() const;


    // -----------------------------------------------------------------
    // Simulation
    // -----------------------------------------------------------------

    bool enableSimulation(bool enabled);

    bool isSimulationEnabled() const;

    bool setSimulatedMeasurements(
        float batteryVoltageVolts,
        float batteryCurrentAmps);

    /**
     * Sets a controlled SoC value for simulation/testing. This does not
     * create a separate planning path: it updates the same SoC state used
     * by updatePowerBudget(). Simulation must already be enabled.
     */
    bool setSimulatedStateOfChargePercent(float stateOfChargePercent);


    // -----------------------------------------------------------------
    // Calibration
    // -----------------------------------------------------------------

    bool setCalibration(const Calibration& calibration);

    Calibration getCalibration() const;


    // -----------------------------------------------------------------
    // Battery State of Charge
    // -----------------------------------------------------------------

    bool initializeStateOfCharge(
        float startingStateOfChargePercent,
        bool restorePersistedState = true);

    bool updateStateOfCharge(float deltaTimeSeconds);

    bool applyVoltageStateOfChargeEstimate(
        float emptyVoltageVolts,
        float fullVoltageVolts,
        float weight);

    float getStateOfChargePercent() const;

    StateOfChargeSource getStateOfChargeSource() const;

    bool isStateOfChargeValid() const;

    bool persistStateOfCharge() const;


    // -----------------------------------------------------------------
    // Power budget
    // -----------------------------------------------------------------

    /**
     * @brief Sets power already committed to Fixed ON loads.
     *
     * PowerManager receives only the watt value.
     * It does not receive Load objects.
     */
    bool setCommittedPowerWatts(float committedPowerWatts);


    /**
     * @brief Sets the remaining hours the user's required-runtime target
     *        still needs, as of this planning cycle.
     *
     * PowerManager itself is wall-clock-agnostic: the caller (Central's
     * runtime orchestration) is responsible for counting down from the
     * configured target using CurrentTimeProvider and passing the
     * updated remaining value in each cycle — see item 13 of the runtime
     * architecture: the target horizon does not stay a static divisor.
     *
     * remainingRequiredRuntimeHours <= 0 disables the runtime-based
     * budget entirely (equivalent to "no target configured") — only the
     * immediate electrical limits then constrain P_available, exactly as
     * before this feature existed.
     *
     * Returns false when the value is not finite or negative.
     */
    bool setRemainingRequiredRuntimeHours(float remainingRequiredRuntimeHours);

    float getRemainingRequiredRuntimeHours() const;


    /**
     * @brief Recalculates the complete central-node power budget.
     *
     * Safe power is constrained by:
     *
     * P_battery_max = V_B * I_battery_max
     *
     * P_main_max = V_B * I_main_max
     *
     * P_limit = min(P_battery_max, P_main_max)
     *
     * P_remaining = max(0, P_limit - P_committed)
     *
     * When a required-runtime target is active (see
     * setRemainingRequiredRuntimeHours()), the usable battery energy
     * above the reserve/minimum SoC additionally bounds P_available:
     *
     * usableSoCFraction = max(0, SoC - minimumSoC) / 100
     *
     * usableEnergyWh = capacityAmpHours * nameplateVoltageVolts * usableSoCFraction
     *
     * sustainableTotalPowerWatts = usableEnergyWh / remainingRequiredRuntimeHours
     *
     * sustainableAvailable = max(0, sustainableTotalPowerWatts - P_committed)
     *
     * P_available = min(P_remaining, sustainableAvailable)
     *
     * If SoC <= minimum SoC:
     *
     * P_available = 0
     */
    bool updatePowerBudget();


    PowerBudget getPowerBudget() const;


    /**
     * @brief Power passed directly to BestFirstSearch.
     */
    float getAvailablePowerWatts() const;


    float getRemainingPowerWatts() const;

    float getCommittedPowerWatts() const;

    float getBatteryMaximumPowerWatts() const;

    float getMainMaximumPowerWatts() const;


    // -----------------------------------------------------------------
    // Diagnostics
    // -----------------------------------------------------------------

    void printDiagnosticReport() const;


private:

    // -----------------------------------------------------------------
    // Hardware
    // -----------------------------------------------------------------

    bool initializeBus();

    bool initializeSensor();

    bool readHardwareMeasurements(PowerMeasurements& measurements) const;

    bool readSimulatedMeasurements(PowerMeasurements& measurements) const;


    // -----------------------------------------------------------------
    // Measurement processing
    // -----------------------------------------------------------------

    static PowerMeasurements applyCalibration(
        const PowerMeasurements& measurements,
        const Calibration& calibration);

    static PowerMeasurements applyExponentialMovingAverage(
        const PowerMeasurements& previous,
        const PowerMeasurements& current,
        float alpha);


    // -----------------------------------------------------------------
    // Validation
    // -----------------------------------------------------------------

    static bool isFinitePositive(float value);

    static bool isValidPercent(float value);

    static bool isValidCalibration(const Calibration& calibration);


    // -----------------------------------------------------------------
    // SoC persistence
    // -----------------------------------------------------------------

    bool loadPersistedStateOfCharge(float& stateOfChargePercent) const;

    bool persistStateOfChargeValue(float stateOfChargePercent) const;

    static float clampStateOfCharge(float value);


    // -----------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------

    BusConfiguration busConfiguration_;

    SensorConfiguration sensorConfiguration_;

    BatteryConfiguration batteryConfiguration_;

    MainBusConfiguration mainBusConfiguration_;

    Calibration calibration_;


    // -----------------------------------------------------------------
    // Measurements
    // -----------------------------------------------------------------

    PowerMeasurements measurements_;

    PowerMeasurements filteredMeasurements_;

    PowerMeasurements simulatedMeasurements_;

    bool hasFilteredMeasurement_;

    MeasurementSource measurementSource_;


    // -----------------------------------------------------------------
    // State of Charge
    // -----------------------------------------------------------------

    float stateOfChargePercent_;

    StateOfChargeSource stateOfChargeSource_;

    bool stateOfChargeInitialized_;


    // -----------------------------------------------------------------
    // Power budget
    // -----------------------------------------------------------------

    PowerBudget powerBudget_;

    /** Remaining hours of the user's required-runtime target; <= 0 disables it. */
    float remainingRequiredRuntimeHours_;


    // -----------------------------------------------------------------
    // Runtime state
    // -----------------------------------------------------------------

    bool initialized_;

    bool simulationEnabled_;


    // -----------------------------------------------------------------
    // ESP-IDF hardware handles
    // -----------------------------------------------------------------

    void* busHandle_;

    void* deviceHandle_;
};


const char* toText(MeasurementSource source);

const char* toText(StateOfChargeSource source);


} // namespace kilowatts

#endif // KILOWATTS_POWER_MANAGER_H