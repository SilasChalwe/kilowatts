/**
 * @file test_available_power_manager.cpp
 * @brief Host-native correctness tests for AvailablePowerManager's power
 *        accounting.
 *
 * Every Total Available Power value used here is a controlled test input,
 * never a value that pretends to come from real hardware — this is the
 * "valid inside a unit test" use permitted by AvailablePowerManager's own
 * README.
 *
 * This file uses a standard host int main(), not an ESP-IDF app_main(), so
 * it can be compiled and run by run_cpp_test.sh's plain g++ invocation —
 * matching test/BestFirstSearch/test_best_first_search.cpp.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
 */

#include "AvailablePowerManager.h"
#include "Load.h"
#include "LoadFilter.h"

#include <cmath>
#include <cstdio>
#include <limits>

using kilowatts::AvailablePowerManager;
using kilowatts::Load;
using kilowatts::LoadFilter;
using kilowatts::LoadMode;
using kilowatts::LoadPower;

namespace {

std::size_t passedChecks = 0U;
std::size_t failedChecks = 0U;

bool nearValue(float actual, float expected, float tolerance = 0.0001F) {
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
    std::printf("%-70s %s\n", name, recordResult(passed));
    return passed;
}

void printSection(const char* title) {
    std::printf("\n======================================================================\n");
    std::printf("%s\n", title);
    std::printf("======================================================================\n");
}

const Load::MacAddress DEMO_MAC = {0x1C, 0xDB, 0xD4, 0x78, 0xE7, 0xB8};

/**
 * TEST 1 - NO FIXED ON LOADS
 */
void testNoFixedOnLoads() {
    printSection("TEST 1 - NO FIXED ON LOADS");

    LoadFilter loadFilter;
    AvailablePowerManager manager;

    const bool accepted = manager.calculateAvailablePower(100.0F, loadFilter);

    reportCheck("A valid Total Available Power is accepted", accepted);
    reportCheck("Total Available Power is stored unchanged", nearValue(manager.getTotalAvailablePowerWatts(), 100.0F));
    reportCheck("Fixed ON Running Power is zero with no Fixed ON Loads",
                nearValue(manager.getFixedOnRunningPowerWatts(), 0.0F));
    reportCheck("Power Available for Auto Loads equals Total Available Power",
                nearValue(manager.getPowerAvailableForAutoLoadsWatts(), 100.0F));
}

/**
 * TEST 2 - ONE FIXED ON LOAD
 */
void testOneFixedOnLoad() {
    printSection("TEST 2 - ONE FIXED ON LOAD");

    Load light(Load::Id{DEMO_MAC, 16U}, "Light", LoadPower{12.0F, 12.0F}, 1U, LoadMode::Fixed::ON);

    LoadFilter loadFilter;
    loadFilter.addLoad(light);

    AvailablePowerManager manager;
    manager.calculateAvailablePower(100.0F, loadFilter);

    reportCheck("Fixed ON Running Power equals the one Load's configured running watts",
                nearValue(manager.getFixedOnRunningPowerWatts(), 12.0F));
    reportCheck("Power Available for Auto Loads is Total minus Fixed ON",
                nearValue(manager.getPowerAvailableForAutoLoadsWatts(), 88.0F));
}

/**
 * TEST 3 - SEVERAL FIXED ON LOADS
 */
void testSeveralFixedOnLoads() {
    printSection("TEST 3 - SEVERAL FIXED ON LOADS");

    Load light(Load::Id{DEMO_MAC, 16U}, "Light", LoadPower{12.0F, 12.0F}, 1U, LoadMode::Fixed::ON);
    Load router(Load::Id{DEMO_MAC, 17U}, "Router", LoadPower{10.0F, 10.0F}, 2U, LoadMode::Fixed::ON);
    Load indicator(Load::Id{DEMO_MAC, 25U}, "StatusIndicator", LoadPower{2.0F, 2.0F}, 1U, LoadMode::Fixed::ON);

    LoadFilter loadFilter;
    loadFilter.addLoad(light);
    loadFilter.addLoad(router);
    loadFilter.addLoad(indicator);

    AvailablePowerManager manager;
    manager.calculateAvailablePower(100.0F, loadFilter);

    reportCheck("Fixed ON Running Power sums every Fixed ON Load's running watts",
                nearValue(manager.getFixedOnRunningPowerWatts(), 24.0F));
    reportCheck("Power Available for Auto Loads reflects the summed Fixed ON power",
                nearValue(manager.getPowerAvailableForAutoLoadsWatts(), 76.0F));
}

/**
 * TEST 4 - FIXED ON POWER BELOW / EQUAL TO / ABOVE TOTAL
 */
void testFixedOnPowerRelativeToTotal() {
    printSection("TEST 4 - FIXED ON POWER BELOW / EQUAL TO / ABOVE TOTAL");

    Load belowLoad(Load::Id{DEMO_MAC, 16U}, "Below", LoadPower{40.0F, 40.0F}, 1U, LoadMode::Fixed::ON);
    LoadFilter belowFilter;
    belowFilter.addLoad(belowLoad);
    AvailablePowerManager belowManager;
    belowManager.calculateAvailablePower(100.0F, belowFilter);
    reportCheck("Fixed ON power below Total leaves a positive remainder",
                nearValue(belowManager.getPowerAvailableForAutoLoadsWatts(), 60.0F));

    Load equalLoad(Load::Id{DEMO_MAC, 16U}, "Equal", LoadPower{100.0F, 100.0F}, 1U, LoadMode::Fixed::ON);
    LoadFilter equalFilter;
    equalFilter.addLoad(equalLoad);
    AvailablePowerManager equalManager;
    equalManager.calculateAvailablePower(100.0F, equalFilter);
    reportCheck("Fixed ON power equal to Total leaves exactly zero",
                nearValue(equalManager.getPowerAvailableForAutoLoadsWatts(), 0.0F));

    Load aboveLoad(Load::Id{DEMO_MAC, 16U}, "Above", LoadPower{150.0F, 150.0F}, 1U, LoadMode::Fixed::ON);
    LoadFilter aboveFilter;
    aboveFilter.addLoad(aboveLoad);
    AvailablePowerManager aboveManager;
    aboveManager.calculateAvailablePower(100.0F, aboveFilter);
    reportCheck("Fixed ON power greater than Total clamps to zero, never negative",
                nearValue(aboveManager.getPowerAvailableForAutoLoadsWatts(), 0.0F));
}

/**
 * TEST 5 - TOTAL AVAILABLE POWER OF ZERO
 */
void testTotalAvailablePowerZero() {
    printSection("TEST 5 - TOTAL AVAILABLE POWER OF ZERO");

    LoadFilter loadFilter;
    AvailablePowerManager manager;

    const bool accepted = manager.calculateAvailablePower(0.0F, loadFilter);

    reportCheck("Zero is a valid Total Available Power", accepted);
    reportCheck("Power Available for Auto Loads is zero when Total is zero",
                nearValue(manager.getPowerAvailableForAutoLoadsWatts(), 0.0F));
}

/**
 * TEST 6 - REJECT NEGATIVE, NaN AND INFINITE TOTAL AVAILABLE POWER
 */
void testRejectInvalidTotalAvailablePower() {
    printSection("TEST 6 - REJECT NEGATIVE, NaN AND INFINITE TOTAL AVAILABLE POWER");

    LoadFilter loadFilter;
    AvailablePowerManager manager;

    reportCheck("A negative Total Available Power is rejected",
                !manager.calculateAvailablePower(-1.0F, loadFilter));

    reportCheck("A NaN Total Available Power is rejected",
                !manager.calculateAvailablePower(std::numeric_limits<float>::quiet_NaN(), loadFilter));

    reportCheck("An infinite Total Available Power is rejected",
                !manager.calculateAvailablePower(std::numeric_limits<float>::infinity(), loadFilter));

    reportCheck("Rejected calls leave Total Available Power at its initial value",
                nearValue(manager.getTotalAvailablePowerWatts(), 0.0F));
    reportCheck("Rejected calls leave Power Available for Auto Loads at its initial value",
                nearValue(manager.getPowerAvailableForAutoLoadsWatts(), 0.0F));
}

/**
 * TEST 7 - FIXED ON TOTAL COMES FROM CONFIGURED RUNNING POWER
 * Fixed ON Running Power must come from LoadPower.runningWatts, and must
 * be recalculated fresh from the LoadFilter on every call rather than
 * accumulated across calls.
 */
void testFixedOnTotalUsesConfiguredRunningPower() {
    printSection("TEST 7 - FIXED ON TOTAL USES CONFIGURED RUNNING POWER, RECALCULATED EACH CALL");

    Load fixedLoad(Load::Id{DEMO_MAC, 16U}, "Heater", LoadPower{20.0F, 30.0F}, 1U, LoadMode::Fixed::ON);

    LoadFilter loadFilter;
    loadFilter.addLoad(fixedLoad);

    AvailablePowerManager manager;
    manager.calculateAvailablePower(100.0F, loadFilter);

    reportCheck("Fixed ON Running Power uses runningWatts (20), not startupWatts (30)",
                nearValue(manager.getFixedOnRunningPowerWatts(), 20.0F));

    LoadFilter emptyFilter;
    manager.calculateAvailablePower(100.0F, emptyFilter);

    reportCheck("A later call with no Fixed ON Loads recalculates Fixed ON Running Power to zero",
                nearValue(manager.getFixedOnRunningPowerWatts(), 0.0F));
}

} // namespace

int main() {
    testNoFixedOnLoads();
    testOneFixedOnLoad();
    testSeveralFixedOnLoads();
    testFixedOnPowerRelativeToTotal();
    testTotalAvailablePowerZero();
    testRejectInvalidTotalAvailablePower();
    testFixedOnTotalUsesConfiguredRunningPower();

    std::printf("\n======================================================================\n");
    std::printf("RESULTS: %zu passed, %zu failed\n", passedChecks, failedChecks);
    std::printf("======================================================================\n");

    return failedChecks == 0U ? 0 : 1;
}
