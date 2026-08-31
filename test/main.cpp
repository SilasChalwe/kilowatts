/**
 * Host-native tests for the clean Kilowatts power-flow model.
 *
 * These tests exercise production PowerManager and BestFirstSearch classes.
 * They do not claim to validate physical INA219 or relay hardware.
 */

#include "BestFirstSearch.h"
#include "CurrentTimeProvider.h"
#include "Load.h"
#include "PowerManager.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace kilowatts;

namespace {

int checks = 0;
int failures = 0;
const char* currentCase = "";

void check(bool condition, const char* description)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("  [FAIL] %s: %s\n", currentCase, description);
    }
}

bool nearlyEqual(float first, float second, float epsilon = 0.05F)
{
    return std::fabs(first - second) <= epsilon;
}

bool prepareBudget(
    PowerManager& powerManager,
    float P_budget,
    float P_reserve,
    float stateOfChargePercent,
    float minimumStateOfChargePercent,
    float remainingRuntimeHours,
    float P_fixed,
    float P_auto,
    float voltage = 15.0F,
    float current = 1.5F)
{
    if (!powerManager.enableSimulation(true)) return false;

    const PowerManager::BusConfiguration bus{0U, 0U, 0U, 100000U};
    const PowerManager::SensorConfiguration sensor{0.1F, 3.0F, 1.0F};
    const PowerManager::BatteryConfiguration battery{
        15.0F,
        200.0F,
        minimumStateOfChargePercent};

    if (!powerManager.initialize(bus, sensor, battery)) return false;
    if (!powerManager.setSimulatedMeasurements(voltage, current)) return false;
    if (!powerManager.updateMeasurements()) return false;
    if (!powerManager.setSimulatedStateOfChargePercent(stateOfChargePercent)) return false;
    if (!powerManager.setPowerBudgetWatts(P_budget)) return false;
    if (!powerManager.setPowerReserveWatts(P_reserve)) return false;
    if (!powerManager.setFixedPowerWatts(P_fixed)) return false;
    if (!powerManager.setAutoPowerWatts(P_auto)) return false;
    if (!powerManager.setRemainingRequiredRuntimeHours(remainingRuntimeHours)) return false;

    return powerManager.updatePowerBudget();
}

Load makeAutoLoad(
    std::uint8_t relayPin,
    const char* name,
    float powerRatingWatts,
    std::uint16_t priority)
{
    const Load::MacAddress mac{{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01}};
    return Load(
        Load::Id{mac, relayPin},
        name,
        powerRatingWatts,
        priority,
        LoadPowerType::DC,
        LoadMode::Auto::OFF);
}

void caseConfiguredBudgetAndReserve()
{
    currentCase = "configured budget and reserve";

    PowerManager powerManager;
    check(
        prepareBudget(
            powerManager,
            200.0F,
            20.0F,
            70.0F,
            0.0F,
            0.0F,
            80.0F,
            0.0F),
        "budget computed");

    PowerBudget budget = powerManager.getPowerBudget();

    check(nearlyEqual(budget.P_budget, 200.0F), "P_budget is 200 W");
    check(nearlyEqual(budget.P_reserve, 20.0F), "P_reserve is 20 W");
    check(nearlyEqual(budget.P_fixed, 80.0F), "P_fixed is 80 W");
    check(
        nearlyEqual(budget.P_auto_available, 100.0F),
        "P_auto_available = 200 - 20 - 80 = 100 W");

    check(powerManager.setAutoPowerWatts(100.0F), "P_auto accepted");
    check(powerManager.updatePowerBudget(), "budget recomputed after AUTO selection");

    budget = powerManager.getPowerBudget();
    check(nearlyEqual(budget.P_auto, 100.0F), "P_auto is 100 W");
    check(
        nearlyEqual(budget.P_remaining, 20.0F),
        "P_remaining = 200 - (80 + 100) = 20 W");
}

void casePartialAutoLeavesHeadroom()
{
    currentCase = "partial AUTO selection";

    PowerManager powerManager;
    check(
        prepareBudget(
            powerManager,
            200.0F,
            20.0F,
            70.0F,
            0.0F,
            0.0F,
            80.0F,
            70.0F),
        "budget computed");

    const PowerBudget budget = powerManager.getPowerBudget();
    check(nearlyEqual(budget.P_auto_available, 100.0F), "P_auto_available remains 100 W");
    check(nearlyEqual(budget.P_remaining, 50.0F), "P_remaining is 50 W");
}

void caseMeasuredPowerIsMonitoringOnly()
{
    currentCase = "P_measured is monitoring";

    PowerManager powerManager;
    check(
        prepareBudget(
            powerManager,
            200.0F,
            20.0F,
            70.0F,
            0.0F,
            0.0F,
            0.0F,
            0.0F,
            15.0F,
            1.5F),
        "budget computed");

    const PowerBudget budget = powerManager.getPowerBudget();
    check(nearlyEqual(budget.P_measured, 22.5F), "P_measured = 15 * 1.5 = 22.5 W");
    check(nearlyEqual(budget.P_budget, 200.0F), "P_budget remains 200 W");
    check(
        nearlyEqual(budget.P_auto_available, 180.0F),
        "P_measured does not replace P_auto_available");
}

void caseRuntimeTargetConstrainsAutoAvailable()
{
    currentCase = "24-hour runtime target";

    PowerManager powerManager;
    check(
        prepareBudget(
            powerManager,
            200.0F,
            20.0F,
            70.0F,
            20.0F,
            24.0F,
            40.0F,
            0.0F),
        "runtime policy computed");

    const PowerBudget budget = powerManager.getPowerBudget();

    // Usable battery energy above the 20% SoC reserve is 1500 Wh.
    // Over 24 hours that internally limits planning to 62.5 W.
    // With 40 W FIXED_ON, Best-First receives 22.5 W.
    check(budget.runtimeBudgetActive, "runtime policy is active");
    check(nearlyEqual(budget.P_auto_available, 22.5F), "P_auto_available is 22.5 W");
    check(budget.requiredRuntimeAchievable, "runtime target is achievable");
}

void caseFixedDemandExceedsRuntimeAllowance()
{
    currentCase = "fixed demand exceeds runtime allowance";

    PowerManager powerManager;
    check(
        prepareBudget(
            powerManager,
            200.0F,
            20.0F,
            70.0F,
            20.0F,
            24.0F,
            80.0F,
            0.0F),
        "runtime policy computed");

    const PowerBudget budget = powerManager.getPowerBudget();
    check(nearlyEqual(budget.P_auto_available, 0.0F), "AUTO receives 0 W");
    check(!budget.requiredRuntimeAchievable, "runtime target is not achievable");
    check(nearlyEqual(budget.P_fixed, 80.0F), "P_fixed remains 80 W");
}

void caseStateOfChargeReserve()
{
    currentCase = "SoC reserve policy";

    PowerManager powerManager;
    check(
        prepareBudget(
            powerManager,
            200.0F,
            20.0F,
            20.0F,
            20.0F,
            24.0F,
            0.0F,
            0.0F),
        "budget computed at reserve");

    const PowerBudget budget = powerManager.getPowerBudget();
    check(nearlyEqual(budget.P_auto_available, 0.0F), "AUTO allocation is zero at SoC reserve");
}

void caseBestFirstReceivesAutoAvailable()
{
    currentCase = "Best-First input unchanged";

    Load loadA = makeAutoLoad(1U, "A", 70.0F, 3U);
    Load loadB = makeAutoLoad(2U, "B", 60.0F, 2U);
    Load loadC = makeAutoLoad(3U, "C", 40.0F, 1U);

    const std::vector<const Load*> automaticLoads{&loadA, &loadB, &loadC};
    CurrentTimeProvider timeProvider;
    const float P_auto_available = 100.0F;

    BestFirstSearch search(P_auto_available, automaticLoads, timeProvider);
    const auto& selected = search.getBestCombination();

    float selectedPower = 0.0F;
    for (const Load* load : selected) {
        if (load != nullptr) selectedPower += load->getPowerRatingWatts();
    }

    check(!selected.empty(), "Best-First selects at least one load");
    check(
        selectedPower <= P_auto_available + 0.01F,
        "selected AUTO power does not exceed P_auto_available");
}

void caseInvalidReserveIsRejected()
{
    currentCase = "invalid P_reserve";

    PowerManager powerManager;
    check(
        !prepareBudget(
            powerManager,
            200.0F,
            220.0F,
            70.0F,
            0.0F,
            0.0F,
            0.0F,
            0.0F),
        "P_reserve greater than P_budget is rejected");
}

} // namespace

int main()
{
    caseConfiguredBudgetAndReserve();
    casePartialAutoLeavesHeadroom();
    caseMeasuredPowerIsMonitoringOnly();
    caseRuntimeTargetConstrainsAutoAvailable();
    caseFixedDemandExceedsRuntimeAllowance();
    caseStateOfChargeReserve();
    caseBestFirstReceivesAutoAvailable();
    caseInvalidReserveIsRejected();

    std::printf(
        "\nKilowatts clean power-flow tests: %d checks, %d failures\n",
        checks,
        failures);

    return failures == 0 ? 0 : 1;
}
