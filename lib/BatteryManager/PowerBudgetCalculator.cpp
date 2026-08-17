/**
 * @file PowerBudgetCalculator.cpp
 * @brief Implements the safe power-budget calculation (Section 4.6.2.3,
 *        Equations 4.9-4.14).
 *
 * Entirely plain, hardware-free math — no ESP_PLATFORM split is needed,
 * unlike most other modules in this project, since there is no hardware
 * access here at all. Always compiled and fully host-testable (see
 * test/PowerBudgetCalculator/).
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
 */

#include "PowerBudgetCalculator.h"

#include <algorithm>
#include <cmath>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

namespace kilowatts {


#ifdef ESP_PLATFORM
static const char *TAG = "POWER_BUDGET_CALCULATOR";
#endif


PowerBudgetCalculator::PowerBudgetCalculator()
    : hasResult_(false),
      ratedEnergyWattHours_(0.0F),
      usableEnergyWattHours_(0.0F),
      runtimePowerWatts_(0.0F),
      maximumBatteryPowerWatts_(0.0F),
      maximumMainPowerWatts_(0.0F),
      availablePowerWatts_(0.0F)
{
}


bool PowerBudgetCalculator::isFinitePercent(float value)
{
    return std::isfinite(value) && value >= 0.0F && value <= 100.0F;
}


bool PowerBudgetCalculator::isFinitePositive(float value)
{
    return std::isfinite(value) && value > 0.0F;
}


bool PowerBudgetCalculator::isFiniteNonNegative(float value)
{
    return std::isfinite(value) && value >= 0.0F;
}


bool PowerBudgetCalculator::calculate(const Inputs& inputs)
{
    if (!isFinitePercent(inputs.stateOfChargePercent) ||
        !isFinitePercent(inputs.minimumStateOfChargePercent) ||
        !isFinitePositive(inputs.nominalBatteryVoltageVolts) ||
        !isFinitePositive(inputs.batteryCapacityAmpHours) ||
        !isFinitePositive(inputs.targetRuntimeHours) ||
        !isFinitePositive(inputs.batteryBusVoltageVolts) ||
        !isFiniteNonNegative(inputs.maximumBatteryDischargeCurrentAmps) ||
        !isFiniteNonNegative(inputs.maximumMainCurrentAmps) ||
        !std::isfinite(inputs.safetyFactor) ||
        inputs.safetyFactor <= 0.0F || inputs.safetyFactor > 1.0F) {
#ifdef ESP_PLATFORM
        ESP_LOGW(TAG, "calculate() rejected: SoC=%.1f%% SoCmin=%.1f%% Vnom=%.2fV CB=%.2fAh Ttarget=%.2fh VB=%.2fV IBmax=%.2fA Imainmax=%.2fA rho=%.3f",
                 static_cast<double>(inputs.stateOfChargePercent),
                 static_cast<double>(inputs.minimumStateOfChargePercent),
                 static_cast<double>(inputs.nominalBatteryVoltageVolts),
                 static_cast<double>(inputs.batteryCapacityAmpHours),
                 static_cast<double>(inputs.targetRuntimeHours),
                 static_cast<double>(inputs.batteryBusVoltageVolts),
                 static_cast<double>(inputs.maximumBatteryDischargeCurrentAmps),
                 static_cast<double>(inputs.maximumMainCurrentAmps),
                 static_cast<double>(inputs.safetyFactor));
#endif
        return false;
    }

    /* E_rated = V_nom * C_B (Equation 4.9). */
    const float ratedEnergyWattHours = inputs.nominalBatteryVoltageVolts * inputs.batteryCapacityAmpHours;

    /*
     * E_usable = E_rated * max(0, (SoC - SoC_min)/100) (Equation 4.10).
     * When SoC <= SoC_min this clamp already forces E_usable, and
     * therefore P_runtime and P_available below, to zero — the same
     * "P_available = 0 for normal Auto-load allocation" rule Section
     * 4.6.2.3 describes, produced by one uniform calculation rather than
     * a separate branch.
     */
    const float usableEnergyWattHours =
        ratedEnergyWattHours * std::max(0.0F, (inputs.stateOfChargePercent - inputs.minimumStateOfChargePercent) / 100.0F);

    /* P_runtime = E_usable / T_target (Equation 4.11). */
    const float runtimePowerWatts = usableEnergyWattHours / inputs.targetRuntimeHours;

    /* P_battery,max = V_B * I_B,max (Equation 4.12). */
    const float maximumBatteryPowerWatts = inputs.batteryBusVoltageVolts * inputs.maximumBatteryDischargeCurrentAmps;

    /* P_main,max = V_B * I_main,max (Equation 4.13). */
    const float maximumMainPowerWatts = inputs.batteryBusVoltageVolts * inputs.maximumMainCurrentAmps;

    /* P_available = rho * min(P_runtime, P_battery,max, P_main,max) (Equation 4.14). */
    const float availablePowerWatts =
        inputs.safetyFactor * std::min(runtimePowerWatts, std::min(maximumBatteryPowerWatts, maximumMainPowerWatts));

    if (!std::isfinite(ratedEnergyWattHours) || !std::isfinite(usableEnergyWattHours) ||
        !std::isfinite(runtimePowerWatts) || !std::isfinite(maximumBatteryPowerWatts) ||
        !std::isfinite(maximumMainPowerWatts) || !std::isfinite(availablePowerWatts)) {
#ifdef ESP_PLATFORM
        ESP_LOGE(TAG, "calculate() produced a non-finite result; rejecting");
#endif
        return false;
    }

    ratedEnergyWattHours_ = ratedEnergyWattHours;
    usableEnergyWattHours_ = usableEnergyWattHours;
    runtimePowerWatts_ = runtimePowerWatts;
    maximumBatteryPowerWatts_ = maximumBatteryPowerWatts;
    maximumMainPowerWatts_ = maximumMainPowerWatts;
    availablePowerWatts_ = availablePowerWatts;
    hasResult_ = true;

#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "Erated=%.2fWh Eusable=%.2fWh Pruntime=%.2fW Pbatmax=%.2fW Pmainmax=%.2fW Pavailable=%.2fW",
             static_cast<double>(ratedEnergyWattHours_),
             static_cast<double>(usableEnergyWattHours_),
             static_cast<double>(runtimePowerWatts_),
             static_cast<double>(maximumBatteryPowerWatts_),
             static_cast<double>(maximumMainPowerWatts_),
             static_cast<double>(availablePowerWatts_));
#endif

    return true;
}


bool PowerBudgetCalculator::hasResult() const
{
    return hasResult_;
}


float PowerBudgetCalculator::getRatedEnergyWattHours() const
{
    return ratedEnergyWattHours_;
}


float PowerBudgetCalculator::getUsableEnergyWattHours() const
{
    return usableEnergyWattHours_;
}


float PowerBudgetCalculator::getRuntimePowerWatts() const
{
    return runtimePowerWatts_;
}


float PowerBudgetCalculator::getMaximumBatteryPowerWatts() const
{
    return maximumBatteryPowerWatts_;
}


float PowerBudgetCalculator::getMaximumMainPowerWatts() const
{
    return maximumMainPowerWatts_;
}


float PowerBudgetCalculator::getAvailablePowerWatts() const
{
    return availablePowerWatts_;
}


} // namespace kilowatts
