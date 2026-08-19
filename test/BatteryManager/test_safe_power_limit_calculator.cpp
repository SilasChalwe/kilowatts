/**
 * @file test_safe_power_limit_calculator.cpp
 * @brief Host-native correctness tests for the power-limit calculation.
 *
 * This file uses a standard host int main(), not an ESP-IDF app_main(), so
 * it can be compiled and run by run_cpp_test.sh's plain g++ invocation.
 */

#include "PowerManager.h"

#include <cmath>
#include <cstdio>
#include <limits>

using kilowatts::SafePowerLimitCalculator;

namespace {

std::size_t passedChecks = 0U;
std::size_t failedChecks = 0U;

bool nearValue(float actual, float expected, float tolerance = 0.01F) {
    return std::fabs(actual - expected) <= tolerance;
}

const char* recordResult(bool passed) {
    if (passed) {
        ++passedChecks;
        return "PASS";
    }

    ++failedChecks;
    return "FAIL";
}

bool reportCheck(const char* name, bool passed) {
    std::printf("%-78s %s\n", name, recordResult(passed));
    return passed;
}

void printSection(const char* title) {
    std::printf("\n======================================================================\n");
    std::printf("%s\n", title);
    std::printf("======================================================================\n");
}

SafePowerLimitCalculator::Inputs makeInputs(
    float stateOfChargePercent,
    float minimumStateOfChargePercent,
    float nominalBatteryVoltageVolts,
    float batteryCapacityAmpHours,
    float targetRuntimeHours,
    float batteryBusVoltageVolts,
    float maximumBatteryDischargeCurrentAmps,
    float maximumMainCurrentAmps,
    float safetyFactor)
{
    SafePowerLimitCalculator::Inputs inputs{};
    inputs.stateOfChargePercent = stateOfChargePercent;
    inputs.minimumStateOfChargePercent = minimumStateOfChargePercent;
    inputs.nominalBatteryVoltageVolts = nominalBatteryVoltageVolts;
    inputs.batteryCapacityAmpHours = batteryCapacityAmpHours;
    inputs.targetRuntimeHours = targetRuntimeHours;
    inputs.batteryBusVoltageVolts = batteryBusVoltageVolts;
    inputs.maximumBatteryDischargeCurrentAmps = maximumBatteryDischargeCurrentAmps;
    inputs.maximumMainCurrentAmps = maximumMainCurrentAmps;
    inputs.safetyFactor = safetyFactor;
    return inputs;
}

/**
 * TEST 1 - INPUT VALIDATION
 */
void testInputValidation() {
    printSection("TEST 1 - INPUT VALIDATION");

    SafePowerLimitCalculator socOutOfRange;
    reportCheck("calculate() rejects SoC above 100",
                !socOutOfRange.calculate(makeInputs(150.0F, 20.0F, 12.0F, 100.0F, 4.0F, 12.0F, 40.0F, 30.0F, 0.9F)));
    reportCheck("A rejected calculate() leaves hasResult() false", !socOutOfRange.hasResult());

    SafePowerLimitCalculator zeroVoltage;
    reportCheck("calculate() rejects a zero nominal battery voltage",
                !zeroVoltage.calculate(makeInputs(80.0F, 20.0F, 0.0F, 100.0F, 4.0F, 12.0F, 40.0F, 30.0F, 0.9F)));

    SafePowerLimitCalculator zeroCapacity;
    reportCheck("calculate() rejects a zero battery capacity",
                !zeroCapacity.calculate(makeInputs(80.0F, 20.0F, 12.0F, 0.0F, 4.0F, 12.0F, 40.0F, 30.0F, 0.9F)));

    SafePowerLimitCalculator zeroTargetRuntime;
    reportCheck("calculate() rejects a zero target runtime",
                !zeroTargetRuntime.calculate(makeInputs(80.0F, 20.0F, 12.0F, 100.0F, 0.0F, 12.0F, 40.0F, 30.0F, 0.9F)));

    SafePowerLimitCalculator zeroBusVoltage;
    reportCheck("calculate() rejects a zero battery bus voltage",
                !zeroBusVoltage.calculate(makeInputs(80.0F, 20.0F, 12.0F, 100.0F, 4.0F, 0.0F, 40.0F, 30.0F, 0.9F)));

    SafePowerLimitCalculator negativeCurrentLimit;
    reportCheck("calculate() rejects a negative maximum battery current",
                !negativeCurrentLimit.calculate(makeInputs(80.0F, 20.0F, 12.0F, 100.0F, 4.0F, 12.0F, -1.0F, 30.0F, 0.9F)));

    SafePowerLimitCalculator zeroSafetyFactor;
    reportCheck("calculate() rejects a zero safety factor",
                !zeroSafetyFactor.calculate(makeInputs(80.0F, 20.0F, 12.0F, 100.0F, 4.0F, 12.0F, 40.0F, 30.0F, 0.0F)));

    SafePowerLimitCalculator safetyFactorAboveOne;
    reportCheck("calculate() rejects a safety factor above 1",
                !safetyFactorAboveOne.calculate(makeInputs(80.0F, 20.0F, 12.0F, 100.0F, 4.0F, 12.0F, 40.0F, 30.0F, 1.5F)));

    SafePowerLimitCalculator nonFinite;
    reportCheck("calculate() rejects a non-finite input",
                !nonFinite.calculate(makeInputs(
                    std::numeric_limits<float>::quiet_NaN(), 20.0F, 12.0F, 100.0F, 4.0F, 12.0F, 40.0F, 30.0F, 0.9F)));

    SafePowerLimitCalculator valid;
    reportCheck("A fully valid input set is accepted",
                valid.calculate(makeInputs(80.0F, 20.0F, 12.0F, 100.0F, 4.0F, 12.0F, 40.0F, 30.0F, 0.9F)));
    reportCheck("hasResult() becomes true after a successful calculate()", valid.hasResult());
}

/**
 * TEST 2 - WORKED EXAMPLE (Equations 4.9-4.14)
 * SoC=80%, SoC_min=20%, V_nom=12V, C_B=100Ah, T_target=4h, V_B=12V,
 * I_B,max=40A, I_main,max=30A, rho=0.9.
 *
 *   E_rated   = 12 * 100 = 1200 Wh
 *   E_usable  = 1200 * max(0, (80-20)/100) = 1200 * 0.6 = 720 Wh
 *   P_runtime = 720 / 4 = 180 W
 *   P_battery,max = 12 * 40 = 480 W
 *   P_main,max    = 12 * 30 = 360 W
 *   P_available   = 0.9 * min(180, 480, 360) = 0.9 * 180 = 162 W
 */
void testWorkedExample() {
    printSection("TEST 2 - WORKED EXAMPLE (Equations 4.9-4.14)");

    SafePowerLimitCalculator calculator;
    reportCheck("calculate() succeeds",
                calculator.calculate(makeInputs(80.0F, 20.0F, 12.0F, 100.0F, 4.0F, 12.0F, 40.0F, 30.0F, 0.9F)));

    reportCheck("E_rated = 12 * 100 = 1200 Wh", nearValue(calculator.getRatedEnergyWattHours(), 1200.0F));
    reportCheck("E_usable = 1200 * 0.6 = 720 Wh", nearValue(calculator.getUsableEnergyWattHours(), 720.0F));
    reportCheck("P_runtime = 720 / 4 = 180 W", nearValue(calculator.getRuntimePowerWatts(), 180.0F));
    reportCheck("P_battery,max = 12 * 40 = 480 W", nearValue(calculator.getMaximumBatteryPowerWatts(), 480.0F));
    reportCheck("P_main,max = 12 * 30 = 360 W", nearValue(calculator.getMaximumMainPowerWatts(), 360.0F));
    reportCheck("P_available = 0.9 * min(180, 480, 360) = 162 W", nearValue(calculator.getAvailablePowerWatts(), 162.0F));
}

/**
 * TEST 3 - EACH LIMIT CAN BE THE BINDING CONSTRAINT
 */
void testEachLimitCanBind() {
    printSection("TEST 3 - EACH LIMIT CAN BE THE BINDING CONSTRAINT");

    /* Runtime-derived power is the smallest: low SoC headroom, generous current limits. */
    SafePowerLimitCalculator runtimeBinds;
    runtimeBinds.calculate(makeInputs(30.0F, 20.0F, 12.0F, 100.0F, 4.0F, 12.0F, 1000.0F, 1000.0F, 1.0F));
    reportCheck("Runtime limit binds when current limits are generous",
                nearValue(runtimeBinds.getAvailablePowerWatts(), runtimeBinds.getRuntimePowerWatts()) &&
                runtimeBinds.getRuntimePowerWatts() < runtimeBinds.getMaximumBatteryPowerWatts() &&
                runtimeBinds.getRuntimePowerWatts() < runtimeBinds.getMaximumMainPowerWatts());

    /* Battery current limit is the smallest: huge capacity/runtime headroom, tiny I_B,max. */
    SafePowerLimitCalculator batteryBinds;
    batteryBinds.calculate(makeInputs(90.0F, 10.0F, 12.0F, 10000.0F, 1000.0F, 12.0F, 5.0F, 1000.0F, 1.0F));
    reportCheck("Battery current limit binds when it is the smallest of the three",
                nearValue(batteryBinds.getAvailablePowerWatts(), batteryBinds.getMaximumBatteryPowerWatts()));

    /* Main-distribution current limit is the smallest. */
    SafePowerLimitCalculator mainBinds;
    mainBinds.calculate(makeInputs(90.0F, 10.0F, 12.0F, 10000.0F, 1000.0F, 12.0F, 1000.0F, 5.0F, 1.0F));
    reportCheck("Main-distribution current limit binds when it is the smallest of the three",
                nearValue(mainBinds.getAvailablePowerWatts(), mainBinds.getMaximumMainPowerWatts()));
}

/**
 * TEST 4 - SoC AT OR BELOW SoC_min PRODUCES ZERO AVAILABLE POWER
 * Section 4.6.2.3: "When SoC <= SoC_min, P_available for normal Auto-load
 * allocation is set to zero." This falls out of Equation 4.10's max(0, ...)
 * clamp without a separate branch, so this is exercised as an ordinary
 * calculate() call rather than special-cased code.
 */
void testAtOrBelowMinimumStateOfChargeYieldsZero() {
    printSection("TEST 4 - SoC <= SoC_min YIELDS ZERO AVAILABLE POWER");

    SafePowerLimitCalculator exactlyAtMinimum;
    exactlyAtMinimum.calculate(makeInputs(20.0F, 20.0F, 12.0F, 100.0F, 4.0F, 12.0F, 40.0F, 30.0F, 0.9F));
    reportCheck("SoC exactly at SoC_min: E_usable = 0", nearValue(exactlyAtMinimum.getUsableEnergyWattHours(), 0.0F));
    reportCheck("SoC exactly at SoC_min: P_available = 0", nearValue(exactlyAtMinimum.getAvailablePowerWatts(), 0.0F));

    SafePowerLimitCalculator belowMinimum;
    belowMinimum.calculate(makeInputs(5.0F, 20.0F, 12.0F, 100.0F, 4.0F, 12.0F, 40.0F, 30.0F, 0.9F));
    reportCheck("SoC below SoC_min: E_usable clamped to 0, not negative",
                nearValue(belowMinimum.getUsableEnergyWattHours(), 0.0F));
    reportCheck("SoC below SoC_min: P_available = 0", nearValue(belowMinimum.getAvailablePowerWatts(), 0.0F));
}

/**
 * TEST 5 - SAFETY FACTOR SCALES THE RESULT
 */
void testSafetyFactorScalesResult() {
    printSection("TEST 5 - SAFETY FACTOR SCALES THE RESULT");

    SafePowerLimitCalculator fullSafetyFactor;
    fullSafetyFactor.calculate(makeInputs(80.0F, 20.0F, 12.0F, 100.0F, 4.0F, 12.0F, 1000.0F, 1000.0F, 1.0F));

    SafePowerLimitCalculator halfSafetyFactor;
    halfSafetyFactor.calculate(makeInputs(80.0F, 20.0F, 12.0F, 100.0F, 4.0F, 12.0F, 1000.0F, 1000.0F, 0.5F));

    reportCheck("Halving the safety factor halves P_available",
                nearValue(halfSafetyFactor.getAvailablePowerWatts(), fullSafetyFactor.getAvailablePowerWatts() / 2.0F));
}

} // namespace

int main() {
    std::printf("KILOWATTS SAFEPOWERLIMITCALCULATOR HOST TEST REPORT\n");

    testInputValidation();
    testWorkedExample();
    testEachLimitCanBind();
    testAtOrBelowMinimumStateOfChargeYieldsZero();
    testSafetyFactorScalesResult();

    printSection("FINAL TEST SUMMARY");
    std::printf("Passed checks: %zu\n", passedChecks);
    std::printf("Failed checks: %zu\n", failedChecks);
    std::printf("OVERALL RESULT: %s\n", failedChecks == 0U ? "PASS" : "FAIL");

    return failedChecks == 0U ? 0 : 1;
}
