/**
 * @file PowerBudgetCalculator.h
 * @brief Declares the safe power-budget calculation (Section 4.6.2.3,
 *        Equations 4.9-4.14).
 *
 * PowerBudgetCalculator's one responsibility: turn the current battery
 * State of Charge and the configured battery/electrical policy into
 * P_available, the total safe planning power for one cycle — exactly the
 * totalAvailablePowerWatts BestFirstSearch expects, and the input
 * AvailablePowerManager subtracts Fixed ON Running Power from to produce
 * Power Available for Auto Loads.
 *
 *   E_rated   = V_nom * C_B                                          (4.9)
 *   E_usable  = E_rated * max(0, (SoC - SoC_min) / 100)              (4.10)
 *   P_runtime = E_usable / T_target                                  (4.11)
 *   P_battery,max = V_B * I_B,max                                    (4.12)
 *   P_main,max    = V_B * I_main,max                                 (4.13)
 *   P_available   = rho * min(P_runtime, P_battery,max, P_main,max)  (4.14)
 *
 * When SoC <= SoC_min, E_usable's max(0, ...) clamp already forces
 * E_usable, and therefore P_runtime and P_available, to zero — this is
 * the same "P_available = 0 for normal Auto-load allocation" rule
 * Section 4.6.2.3 states explicitly, falling out of the equations above
 * rather than needing a separate branch, so exactly one calculation path
 * produces a consistent P_available for the whole planning cycle.
 *
 * This class does not read INA219 hardware itself (the caller supplies
 * the already-measured/filtered battery bus voltage), does not calculate
 * SoC (BatteryStateOfCharge), does not perform Fixed-Load allocation
 * (AvailablePowerManager), and does not run Best-First Search.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
 */

#ifndef KILOWATTS_POWER_BUDGET_CALCULATOR_H
#define KILOWATTS_POWER_BUDGET_CALCULATOR_H

namespace kilowatts {


class PowerBudgetCalculator {

public:

    /**
     * Every input the safe power-budget calculation needs for one
     * planning cycle. Battery policy fields (minimumStateOfChargePercent,
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
        float stateOfChargePercent;              // SoC        (Equation 4.10)
        float minimumStateOfChargePercent;        // SoC_min    (Equation 4.10)
        float nominalBatteryVoltageVolts;         // V_nom      (Equation 4.9)
        float batteryCapacityAmpHours;            // C_B        (Equation 4.9)
        float targetRuntimeHours;                 // T_target   (Equation 4.11)
        float batteryBusVoltageVolts;             // V_B        (Equations 4.12-4.13)
        float maximumBatteryDischargeCurrentAmps; // I_B,max    (Equation 4.12)
        float maximumMainCurrentAmps;             // I_main,max (Equation 4.13)
        float safetyFactor;                       // rho, 0 < rho <= 1 (Equation 4.14)
    };


    PowerBudgetCalculator();


    /**
     * Runs Equations 4.9-4.14 over inputs and stores the result.
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


    /** E_rated: rated battery energy in watt-hours (Equation 4.9). */
    float getRatedEnergyWattHours() const;

    /** E_usable: usable energy above the protected SoC_min reserve (Equation 4.10). */
    float getUsableEnergyWattHours() const;

    /** P_runtime: sustainable power to preserve T_target hours of runtime (Equation 4.11). */
    float getRuntimePowerWatts() const;

    /** P_battery,max: battery-current-derived power limit (Equation 4.12). */
    float getMaximumBatteryPowerWatts() const;

    /** P_main,max: main-distribution-current-derived power limit (Equation 4.13). */
    float getMaximumMainPowerWatts() const;

    /**
     * P_available: the total safe planning power for this cycle
     * (Equation 4.14) — exactly
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


} // namespace kilowatts

#endif // KILOWATTS_POWER_BUDGET_CALCULATOR_H
