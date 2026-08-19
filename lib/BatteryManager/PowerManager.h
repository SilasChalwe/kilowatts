/**
 * @file PowerManager.h
 * @brief Declares the three pure-logic steps that turn a battery reading
 *        into how much power is left for new Auto Loads this cycle.
 *
 * INA219Monitor is the only hardware/I2C-dependent class in BatteryManager
 * and stays in its own file. Everything else is plain math with no
 * hardware dependency, so it lives here as one pipeline, read top to
 * bottom in the order it actually runs:
 *
 *   BatteryStateOfCharge   — coulomb-counts the filtered battery current
 *                             from INA219Monitor into a SoC percentage.
 *   SafePowerLimitCalculator — turns that SoC percentage and the
 *                             configured battery/electrical policy into
 *                             the total safe power for this cycle.
 *   AvailablePowerManager  — subtracts what Fixed ON Loads already commit
 *                             from that total, leaving what Best-First
 *                             Search may offer to Auto Loads.
 *
 * None of the three reads INA219 hardware directly, classifies Loads
 * (LoadFilter), or runs Best-First Search.
 */

#ifndef KILOWATTS_POWER_MANAGER_H
#define KILOWATTS_POWER_MANAGER_H

#include <cstdint>

#include "LoadFilter.h"

namespace kilowatts {


/**
 * Where the current SoC estimate actually came from — never collapsed
 * into the bare percentage itself, since a caller/log/MQTT payload must
 * never present a defaulted or never-established estimate as if it were
 * a genuine measurement.
 *
 *   UNKNOWN                — this object has never been initialize()d (for
 *                            example: true factory state, or no battery
 *                            sensor has ever been commissioned on this
 *                            Node, so nothing ever called initialize()).
 *   PERSISTED              — restored from a genuinely persisted NVS record.
 *   INITIAL_COMMISSIONING  — no persisted record existed; initialize()'s own
 *                            configured starting estimate was used instead.
 *   COULOMB_COUNTING       — update() has actually advanced the estimate
 *                            from real measured battery current at least
 *                            once this boot, on top of either of the above.
 */
enum class StateOfChargeSource : std::uint8_t {
    UNKNOWN = 0U,
    PERSISTED = 1U,
    INITIAL_COMMISSIONING = 2U,
    COULOMB_COUNTING = 3U
};

const char* toText(StateOfChargeSource source);


/**
 * Coulomb-counting battery State of Charge estimator:
 *
 *   SoC(t) = clamp[ SoC(t-Dt) - 100 * I_B(t) * Dt / (3600 * C_B), 0, 100 ]
 *
 * where I_B(t) is the battery current in amperes with discharge defined
 * as positive (so charging, a negative I_B, increases the SoC estimate),
 * Dt is the elapsed measurement interval in seconds, and C_B is the
 * configured battery capacity in ampere-hours.
 */
class BatteryStateOfCharge {

public:

    BatteryStateOfCharge();


    /**
     * Configures the battery's rated capacity C_B (ampere-hours) and
     * establishes the starting SoC estimate. On the ESP32 target, a
     * valid persisted SoC from a previous run (see persist()) takes
     * precedence over defaultStateOfChargePercent — getSource() reports
     * StateOfChargeSource::PERSISTED in that case. Otherwise
     * defaultStateOfChargePercent (a real, deliberate configured starting
     * assumption, not a fabricated reading) is used, and getSource()
     * reports StateOfChargeSource::INITIAL_COMMISSIONING. A host build
     * always takes the latter path, since it has no persistent storage.
     *
     * A Node with no battery sensor commissioned should never call this,
     * so getStateOfChargePercent()/isValid() keep honestly reporting
     * UNKNOWN/invalid rather than presenting an unbacked default as real.
     *
     * Rejected (returns false, this object stays uninitialized/UNKNOWN)
     * when batteryCapacityAmpHours is not a finite positive value, or
     * defaultStateOfChargePercent is outside [0, 100].
     */
    bool initialize(float batteryCapacityAmpHours, float defaultStateOfChargePercent);


    /** Returns true once initialize() has succeeded. */
    bool isInitialized() const;

    /** Equivalent to getSource() != StateOfChargeSource::UNKNOWN — whether getStateOfChargePercent() currently reflects a real estimate. */
    bool isValid() const;

    /** StateOfChargeSource::UNKNOWN until initialize() first succeeds. */
    StateOfChargeSource getSource() const;


    /**
     * Advances the SoC estimate by coulomb counting using the measured
     * battery-bus current and the elapsed interval since the previous
     * update. On success, getSource() becomes
     * StateOfChargeSource::COULOMB_COUNTING.
     *
     * Rejected (returns false, the SoC estimate/source left unchanged)
     * when this object is not initialized, batteryCurrentAmps is not
     * finite, or deltaTimeSeconds is not a finite positive value.
     */
    bool update(float batteryCurrentAmps, float deltaTimeSeconds);


    /**
     * Returns the current SoC estimate, a percentage in [0, 100]. Always
     * check isValid() first — an invalid (UNKNOWN-source) estimate is
     * still returned as a real-looking float (0.0, this object's
     * constructed default) purely so callers have a well-defined value to
     * format, never as a claim that the battery is genuinely at 0%.
     */
    float getStateOfChargePercent() const;


    /**
     * Persists the current SoC estimate to NVS (ESP32 target only) so
     * the next boot's initialize() call can resume from it rather than
     * defaultStateOfChargePercent. Intended to be called periodically
     * (for example once per Optimisation Task cycle) and is deliberately
     * not called automatically by every update(), so the caller controls
     * how often flash is written.
     *
     * Returns false on a host build, or when the underlying NVS write
     * failed; the in-memory estimate is unaffected either way.
     */
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


/**
 * Turns a SoC percentage and the configured battery/electrical policy
 * into the total safe power for one planning cycle:
 *
 *   E_rated       = V_nom * C_B
 *   E_usable      = E_rated * max(0, (SoC - SoC_min) / 100)
 *   P_runtime     = E_usable / T_target
 *   P_battery,max = V_B * I_B,max
 *   P_main,max    = V_B * I_main,max
 *   P_available   = rho * min(P_runtime, P_battery,max, P_main,max)
 *
 * When SoC <= SoC_min, E_usable's max(0, ...) clamp already forces
 * E_usable, and therefore P_runtime and P_available, to zero — this falls
 * out of the equations above rather than needing a separate branch, so
 * exactly one calculation path produces a consistent P_available for the
 * whole planning cycle.
 */
class SafePowerLimitCalculator {

public:

    /**
     * Every input the power-limit calculation needs for one planning
     * cycle. Battery policy fields (minimumStateOfChargePercent,
     * nominalBatteryVoltageVolts, batteryCapacityAmpHours,
     * targetRuntimeHours, maximumBatteryDischargeCurrentAmps,
     * maximumMainCurrentAmps, safetyFactor) are configuration, supplied by
     * the caller from CentralNodeConfig — this class never invents a
     * default for them. stateOfChargePercent and batteryBusVoltageVolts
     * are this cycle's measured/estimated values
     * (BatteryStateOfCharge::getStateOfChargePercent(), the central
     * INA219's filtered battery-bus voltage).
     */
    struct Inputs {
        float stateOfChargePercent;              // SoC
        float minimumStateOfChargePercent;        // SoC_min
        float nominalBatteryVoltageVolts;         // V_nom
        float batteryCapacityAmpHours;            // C_B
        float targetRuntimeHours;                 // T_target
        float batteryBusVoltageVolts;             // V_B
        float maximumBatteryDischargeCurrentAmps; // I_B,max
        float maximumMainCurrentAmps;             // I_main,max
        float safetyFactor;                       // rho, 0 < rho <= 1
    };


    SafePowerLimitCalculator();


    /**
     * Runs the power-limit calculation over inputs and stores the result.
     *
     * Rejected (returns false, the previous result if any is left
     * unchanged) when any field of inputs is not finite, when
     * stateOfChargePercent/minimumStateOfChargePercent is outside
     * [0, 100], or when nominalBatteryVoltageVolts,
     * batteryCapacityAmpHours, targetRuntimeHours or
     * batteryBusVoltageVolts is not strictly positive, or when
     * maximumBatteryDischargeCurrentAmps/maximumMainCurrentAmps is
     * negative, or safetyFactor is outside (0, 1].
     */
    bool calculate(const Inputs& inputs);


    /** Returns true once calculate() has succeeded at least once. */
    bool hasResult() const;


    /** E_rated: rated battery energy in watt-hours. */
    float getRatedEnergyWattHours() const;

    /** E_usable: usable energy above the protected SoC_min reserve. */
    float getUsableEnergyWattHours() const;

    /** P_runtime: sustainable power to preserve T_target hours of runtime. */
    float getRuntimePowerWatts() const;

    /** P_battery,max: battery-current-derived power limit. */
    float getMaximumBatteryPowerWatts() const;

    /** P_main,max: main-distribution-current-derived power limit. */
    float getMaximumMainPowerWatts() const;

    /**
     * P_available: the total safe planning power for this cycle — exactly
     * BestFirstSearch::ElectricalPlanningState::totalAvailablePowerWatts
     * and AvailablePowerManager::calculateAvailablePower()'s
     * totalAvailablePowerWatts argument.
     */
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


/**
 * Subtracts what Fixed ON Loads already commit from
 * SafePowerLimitCalculator's total, leaving what Best-First Search may
 * offer to Auto Loads. Maintains three clearly named values:
 *
 * - Total Available Power: supplied by the caller (SafePowerLimitCalculator's
 *   result) — this class does not measure or calculate it.
 * - Fixed ON Running Power: the total configured Running Power
 *   (LoadPower::runningWatts) already required by every Load currently
 *   classified as Fixed::ON, recalculated from scratch on every call by
 *   traversing an already-classified LoadFilter.
 * - Power Available for Auto Loads: Total Available Power minus Fixed ON
 *   Running Power, clamped so it never becomes negative.
 *
 * Does not read INA219 hardware, does not calculate SoC or the safe
 * power limit, does not classify Loads (LoadFilter), and does not run
 * Best-First Search.
 */
class AvailablePowerManager {

public:

    AvailablePowerManager();


    /**
     * Calculates Power Available for Auto Loads from an externally
     * supplied Total Available Power and the Fixed ON Loads already
     * classified by loadFilter.
     *
     * Rejected (returns false, all three values keep whatever they were
     * before this call) when totalAvailablePowerWatts is negative, NaN or
     * infinite. Zero is accepted.
     */
    bool calculateAvailablePower(
        float totalAvailablePowerWatts,
        const LoadFilter& loadFilter
    );


    /** Returns the unchanged value most recently supplied and accepted. */
    float getTotalAvailablePowerWatts() const;

    /** Returns the Running Power committed by every Fixed ON Load. */
    float getFixedOnRunningPowerWatts() const;

    /**
     * Returns Total Available Power minus Fixed ON Running Power, never
     * less than zero.
     */
    float getPowerAvailableForAutoLoadsWatts() const;


private:

    float totalAvailablePowerWatts_;

    float fixedOnRunningPowerWatts_;

    float powerAvailableForAutoLoadsWatts_;
};


} // namespace kilowatts

#endif // KILOWATTS_POWER_MANAGER_H
