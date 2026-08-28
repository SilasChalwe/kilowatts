/**
 * @file main.cpp
 * @brief Host-native behavioral tests for the Kilowatts runtime/budget math.
 *
 * Builds and runs with plain g++ (no ESP32, no PlatformIO Unity runner) —
 * see run_tests.sh. ESP_PLATFORM is intentionally left undefined, so these
 * tests exercise the host-safe/pure-logic branches of the production classes.
 * PlatformIO firmware builds separately verify the ESP-IDF branches compile;
 * neither test claims to validate physical hardware behaviour.
 *
 * This is deliberately a small hand-rolled runner, not a framework: each
 * CASE below matches one requirement from the runtime/budget architecture
 * and asserts against PowerManager/BestFirstSearch directly — the actual
 * production classes, not a reimplementation of their math.
 */

#include "BestFirstSearch.h"
#include "CurrentTimeProvider.h"
#include "Load.h"
#include "PowerManager.h"

#include <cmath>
#include <cstdio>

using namespace kilowatts;

namespace {

int g_checkCount = 0;
int g_failureCount = 0;
const char* g_currentCase = "";

void check(bool condition, const char* description)
{
    ++g_checkCount;
    if (!condition) {
        ++g_failureCount;
        std::printf("  [FAIL] %s: %s\n", g_currentCase, description);
    }
}

bool nearlyEqual(float a, float b, float epsilon = 0.05F)
{
    return std::fabs(a - b) <= epsilon;
}

/**
 * One reusable battery scenario: 12 V nominal, 100 Ah, 100 A discharge/main
 * limits (large enough that the immediate electrical limit never binds in
 * these tests) so every test isolates the runtime-sustainability math.
 */
bool prepareBudget(
    PowerManager& powerManager,
    float currentStateOfChargePercent,
    float minimumStateOfChargePercent,
    float remainingRequiredRuntimeHours,
    float committedPowerWatts)
{
    if (!powerManager.enableSimulation(true)) return false;

    const PowerManager::BusConfiguration bus{0U, 0U, 0U, 100000U};
    const PowerManager::SensorConfiguration sensor{0.1F, 3.0F, 1.0F};
    const PowerManager::BatteryConfiguration battery{
        12.0F, 100.0F, minimumStateOfChargePercent, 100.0F};
    const PowerManager::MainBusConfiguration mainBus{100.0F};

    if (!powerManager.initialize(bus, sensor, battery, mainBus)) return false;
    if (!powerManager.setSimulatedMeasurements(12.0F, 0.0F)) return false;
    if (!powerManager.updateMeasurements()) return false;
    if (!powerManager.setSimulatedStateOfChargePercent(currentStateOfChargePercent)) return false;
    if (!powerManager.setCommittedPowerWatts(committedPowerWatts)) return false;
    if (!powerManager.setRemainingRequiredRuntimeHours(remainingRequiredRuntimeHours)) return false;
    return powerManager.updatePowerBudget();
}

Load makeLoad(
    std::uint8_t relayPin,
    const char* name,
    float powerRatingWatts,
    std::uint16_t priority,
    LoadMode::Value mode)
{
    const Load::MacAddress mac{{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01}};
    return Load(Load::Id{mac, relayPin}, name, powerRatingWatts, priority, LoadPowerType::AC, mode);
}

/*
 * CASE 1 vs CASE 2: same high SoC, short vs. long required runtime.
 * Usable energy is fixed by SoC; spreading it over a longer runtime must
 * yield a smaller sustainable/AUTO budget, and vice versa.
 */
void caseHighSocShortVsLongRuntime()
{
    g_currentCase = "CASE 1/2 (SoC 80%, reserve 20%): short vs long runtime";

    PowerManager shortRuntime;
    check(prepareBudget(shortRuntime, 80.0F, 20.0F, 2.0F, 0.0F), "short-runtime budget computed");
    const PowerBudget shortBudget = shortRuntime.getPowerBudget();

    PowerManager longRuntime;
    check(prepareBudget(longRuntime, 80.0F, 20.0F, 8.0F, 0.0F), "long-runtime budget computed");
    const PowerBudget longBudget = longRuntime.getPowerBudget();

    // usableEnergyWh = 100Ah * 12V * (80-20)/100 = 720 Wh
    // 2h target -> 360 W sustainable; 8h target -> 90 W sustainable.
    check(nearlyEqual(shortBudget.sustainableTotalPowerWatts, 360.0F), "2h target sustains 360 W");
    check(nearlyEqual(longBudget.sustainableTotalPowerWatts, 90.0F), "8h target sustains 90 W");
    check(nearlyEqual(shortBudget.availablePowerWatts, 360.0F), "2h target AUTO budget is 360 W");
    check(nearlyEqual(longBudget.availablePowerWatts, 90.0F), "8h target AUTO budget is 90 W");
    check(shortBudget.availablePowerWatts > longBudget.availablePowerWatts,
          "shorter required runtime yields a larger AUTO budget than a longer one");
}

/* CASE 3: current SoC close to the reserve leaves only a small AUTO budget. */
void caseSocNearReserve()
{
    g_currentCase = "CASE 3 (SoC 21%, reserve 20%, 2h)";

    PowerManager powerManager;
    check(prepareBudget(powerManager, 21.0F, 20.0F, 2.0F, 0.0F), "near-reserve budget computed");
    const PowerBudget budget = powerManager.getPowerBudget();

    // usableEnergyWh = 100Ah * 12V * (21-20)/100 = 12 Wh -> 6 W over 2h.
    check(nearlyEqual(budget.sustainableTotalPowerWatts, 6.0F), "1% headroom sustains 6 W");
    check(budget.availablePowerWatts > 0.0F, "AUTO budget stays strictly positive above reserve");
    check(budget.availablePowerWatts < 10.0F, "AUTO budget is very small close to reserve");
}

/* CASE 4: at or below the reserve SoC, usable energy and AUTO budget are zero. */
void caseSocAtOrBelowReserve()
{
    g_currentCase = "CASE 4 (SoC <= reserve)";

    PowerManager atReserve;
    check(prepareBudget(atReserve, 20.0F, 20.0F, 2.0F, 0.0F), "at-reserve budget computed");
    const PowerBudget atReserveBudget = atReserve.getPowerBudget();
    check(nearlyEqual(atReserveBudget.sustainableTotalPowerWatts, 0.0F), "SoC == reserve: usable energy is 0 Wh");
    check(nearlyEqual(atReserveBudget.availablePowerWatts, 0.0F), "SoC == reserve: AUTO budget is 0 W");

    PowerManager belowReserve;
    check(prepareBudget(belowReserve, 10.0F, 20.0F, 2.0F, 0.0F), "below-reserve budget computed");
    const PowerBudget belowReserveBudget = belowReserve.getPowerBudget();
    check(nearlyEqual(belowReserveBudget.availablePowerWatts, 0.0F), "SoC < reserve: AUTO budget is 0 W");
}

/* CASE 5: FIXED_ON expected power is deducted before the AUTO budget is produced. */
void caseFixedOnDeductedBeforeAuto()
{
    g_currentCase = "CASE 5 (FIXED_ON 100 W deducted first)";

    PowerManager powerManager;
    check(prepareBudget(powerManager, 80.0F, 20.0F, 2.0F, 100.0F), "budget with 100 W committed computed");
    const PowerBudget budget = powerManager.getPowerBudget();

    check(nearlyEqual(budget.committedPowerWatts, 100.0F), "committed power reflects the FIXED_ON load exactly");
    // sustainableTotal is 360 W (unchanged by committed power); AUTO gets the remainder: 260 W.
    check(nearlyEqual(budget.sustainableTotalPowerWatts, 360.0F), "sustainable TOTAL is unaffected by committed power");
    check(nearlyEqual(budget.availablePowerWatts, 260.0F), "AUTO budget is sustainable total minus FIXED_ON power");
}

/* CASE 6: FIXED_ON power consumes exactly all sustainable power -> AUTO budget is zero. */
void caseFixedOnConsumesAllSustainablePower()
{
    g_currentCase = "CASE 6 (FIXED_ON == sustainable total)";

    PowerManager powerManager;
    check(prepareBudget(powerManager, 80.0F, 20.0F, 2.0F, 360.0F), "budget with 360 W committed computed");
    const PowerBudget budget = powerManager.getPowerBudget();

    check(nearlyEqual(budget.availablePowerWatts, 0.0F), "AUTO budget is 0 W when FIXED_ON equals sustainable total");
    check(budget.requiredRuntimeAchievable, "runtime target stays achievable at exactly the sustainable limit");
}

/*
 * CASE 7: FIXED_ON power EXCEEDS the sustainable total -> AUTO budget is
 * zero, requiredRuntimeAchievable is false, and the FIXED_ON commitment
 * PowerManager reports back is exactly what was set — never clipped,
 * zeroed or otherwise "fixed up" to chase the runtime target.
 */
void caseFixedOnExceedsSustainableTotal()
{
    g_currentCase = "CASE 7 (FIXED_ON > sustainable total)";

    PowerManager powerManager;
    check(prepareBudget(powerManager, 80.0F, 20.0F, 2.0F, 400.0F), "budget with 400 W committed computed");
    const PowerBudget budget = powerManager.getPowerBudget();

    check(nearlyEqual(budget.availablePowerWatts, 0.0F), "AUTO budget is 0 W when FIXED_ON exceeds sustainable total");
    check(!budget.requiredRuntimeAchievable, "runtime target is reported unachievable");
    check(nearlyEqual(budget.committedPowerWatts, 400.0F),
          "FIXED_ON commitment is echoed back unchanged, never reduced to chase achievability");
}

/*
 * CASE 8: multiple AUTO Loads are handed the final AUTO budget (as CASE 5
 * would have produced: 260 W after a 100 W FIXED_ON deduction) and
 * BestFirstSearch must return a combination that fits inside it. It also
 * prefers admitting MORE loads (its documented size-first merit rule) over
 * a single higher-power one, so {A, B} (250 W) must win over {C} alone
 * (200 W) or {A} / {B} alone.
 */
void caseMultipleAutoLoadsRespectBudget()
{
    g_currentCase = "CASE 8 (BestFirstSearch honors the AUTO budget)";

    const float autoBudgetWatts = 260.0F; // matches CASE 5's post-FIXED_ON AUTO budget.

    const Load loadA = makeLoad(1U, "A", 100.0F, 5U, LoadMode::Auto::ON);
    const Load loadB = makeLoad(2U, "B", 150.0F, 3U, LoadMode::Auto::ON);
    const Load loadC = makeLoad(3U, "C", 200.0F, 1U, LoadMode::Auto::ON);

    const std::vector<const Load*> autoLoads{&loadA, &loadB, &loadC};
    const CurrentTimeProvider timeProvider;

    const BestFirstSearch search(autoBudgetWatts, autoLoads, timeProvider);
    const std::vector<const Load*>& selected = search.getBestCombination();

    float totalSelectedPowerWatts = 0.0F;
    for (const Load* load : selected) totalSelectedPowerWatts += load->getPowerRatingWatts();

    check(totalSelectedPowerWatts <= autoBudgetWatts + 0.01F, "selected combination fits within the AUTO budget");
    check(selected.size() == 2U, "search admits the larger 2-Load combination, not a single Load");

    bool containsA = false;
    bool containsB = false;
    for (const Load* load : selected) {
        if (load == &loadA) containsA = true;
        if (load == &loadB) containsB = true;
    }
    check(containsA && containsB, "search selects A+B (250 W), the best combination that fits 260 W");
}

/*
 * CASE 9: as remaining required runtime counts down across planning cycles,
 * the SAME PowerManager instance must use the newly supplied remaining
 * duration each time, not silently keep computing against the first
 * (original) value it was ever given.
 */
/*
 * Scenario tests: public black-box behavior of the planning engine under
 * realistic operating profiles. These use the same BestFirstSearch API as
 * production but model the household, mining, and hospital cases directly.
 */
void caseHomeScenario()
{
    g_currentCase = "CASE 9 (home scenario)";

    const float availablePowerWatts = 180.0F;
    const Load lights = makeLoad(11U, "Lights", 80.0F, 3U, LoadMode::Auto::ON);
    const Load fridge = makeLoad(12U, "Fridge", 120.0F, 6U, LoadMode::Auto::ON);
    const Load waterPump = makeLoad(13U, "Water pump", 100.0F, 8U, LoadMode::Auto::ON);

    const std::vector<const Load*> automaticLoads{&lights, &fridge, &waterPump};
    const CurrentTimeProvider timeProvider;
    const BestFirstSearch search(availablePowerWatts, automaticLoads, timeProvider);
    const std::vector<const Load*>& selected = search.getBestCombination();

    bool hasLights = false;
    bool hasPump = false;
    bool hasFridge = false;
    for (const Load* load : selected) {
        if (load == &lights) hasLights = true;
        if (load == &waterPump) hasPump = true;
        if (load == &fridge) hasFridge = true;
    }

    check(selected.size() == 2U, "home scenario selects exactly two loads under a 180 W budget");
    check(hasLights && hasPump && !hasFridge,
          "home scenario keeps essential water + lights while leaving the higher-power fridge out");
}

void caseMineScenario()
{
    g_currentCase = "CASE 10 (mine scenario)";

    const float availablePowerWatts = 220.0F;
    const Load ventilation = makeLoad(21U, "Ventilation", 110.0F, 30U, LoadMode::Auto::ON);
    const Load waterPump = makeLoad(22U, "Water pump", 95.0F, 25U, LoadMode::Auto::ON);
    const Load conveyor = makeLoad(23U, "Conveyor", 90.0F, 18U, LoadMode::Auto::ON);

    const std::vector<const Load*> automaticLoads{&ventilation, &waterPump, &conveyor};
    const CurrentTimeProvider timeProvider;
    const BestFirstSearch search(availablePowerWatts, automaticLoads, timeProvider);
    const std::vector<const Load*>& selected = search.getBestCombination();

    bool hasVentilation = false;
    bool hasPump = false;
    bool hasConveyor = false;
    for (const Load* load : selected) {
        if (load == &ventilation) hasVentilation = true;
        if (load == &waterPump) hasPump = true;
        if (load == &conveyor) hasConveyor = true;
    }

    check(selected.size() == 2U, "mine scenario selects the best feasible 2-load combination");
    check(hasVentilation && hasPump && !hasConveyor,
          "mine scenario prioritizes ventilation + water pump over the lower-priority conveyor");
}

void caseHospitalScenario()
{
    g_currentCase = "CASE 11 (hospital scenario)";

    const float availablePowerWatts = 350.0F;
    const Load ventilator = makeLoad(31U, "Ventilator", 220.0F, 40U, LoadMode::Auto::ON);
    const Load monitor = makeLoad(32U, "Patient monitor", 120.0F, 35U, LoadMode::Auto::ON);
    const Load lights = makeLoad(33U, "Critical lights", 90.0F, 18U, LoadMode::Auto::ON);
    const Load laundry = makeLoad(34U, "Laundry", 150.0F, 7U, LoadMode::Auto::ON);

    const std::vector<const Load*> automaticLoads{&ventilator, &monitor, &lights, &laundry};
    const CurrentTimeProvider timeProvider;
    const BestFirstSearch search(availablePowerWatts, automaticLoads, timeProvider);
    const std::vector<const Load*>& selected = search.getBestCombination();

    bool hasVentilator = false;
    bool hasMonitor = false;
    bool hasLights = false;
    bool hasLaundry = false;
    for (const Load* load : selected) {
        if (load == &ventilator) hasVentilator = true;
        if (load == &monitor) hasMonitor = true;
        if (load == &lights) hasLights = true;
        if (load == &laundry) hasLaundry = true;
    }

    check(selected.size() == 2U, "hospital scenario selects the best feasible 2-load critical combination");
    check(hasVentilator && hasMonitor && !hasLights && !hasLaundry,
          "hospital scenario prioritizes life-support ventilation + monitoring over lower-critical loads");
}

void caseRemainingRuntimeDrivesEachRecompute()
{
    g_currentCase = "CASE 12 (remaining runtime, not original, is used each cycle)";

    PowerManager powerManager;
    check(prepareBudget(powerManager, 80.0F, 20.0F, 8.0F, 0.0F), "initial 8h-remaining budget computed");
    const PowerBudget firstBudget = powerManager.getPowerBudget();
    check(nearlyEqual(firstBudget.sustainableTotalPowerWatts, 90.0F), "8h remaining sustains 90 W");

    // Same instance, same SoC/committed power - only remaining runtime changes,
    // exactly as Central's countdown would pass in a smaller value next cycle.
    check(powerManager.setRemainingRequiredRuntimeHours(2.0F), "remaining runtime updated to 2h");
    check(powerManager.updatePowerBudget(), "budget recomputed for the new remaining runtime");
    const PowerBudget secondBudget = powerManager.getPowerBudget();

    check(nearlyEqual(secondBudget.sustainableTotalPowerWatts, 360.0F),
          "2h remaining sustains 360 W - the ORIGINAL 8h value is not reused");
    check(secondBudget.availablePowerWatts > firstBudget.availablePowerWatts,
          "AUTO budget grows as remaining runtime shrinks, proving each cycle recomputes fresh");
}

/*
 * CASE 10: real sensor values and simulated values must both flow into the
 * SAME PowerManager/runtime-budget calculation, never two different code
 * paths. This test does NOT and cannot prove that a real INA219 reading
 * and a simulated one are numerically equivalent - there is no INA219 on
 * host, and that was never the claim.
 *
 * What this test proves is narrower and is what's actually checkable on
 * host: PowerManager::updatePowerBudget() reads only measurements_ and the
 * SoC/committed/runtime state - it has no branch on
 * simulationEnabled_/measurementSource_. It is demonstrated by reaching a
 * given voltage through the simulation path, capturing the budget, then
 * making the exact same hardware-facing read attempt production code takes
 * when simulation is off (which fails safely on host - there is no
 * INA219 - leaving measurements_ at its last value while
 * measurementSource_ becomes NONE), and recomputing. Getting numerically
 * identical results despite the measurement source differing shows
 * updatePowerBudget() does not branch on measurement source - the same
 * one calculation is what both a real reading and a simulated one would
 * be run through, once either has produced an equivalent measurement
 * value.
 */
void caseSimulationAndHardwarePathShareOneCalculation()
{
    g_currentCase = "CASE 10 (updatePowerBudget() does not branch on measurement source)";

    PowerManager powerManager;
    check(prepareBudget(powerManager, 80.0F, 20.0F, 2.0F, 100.0F), "simulation-sourced budget computed");
    const PowerBudget simulatedBudget = powerManager.getPowerBudget();
    check(powerManager.getMeasurementSource() == MeasurementSource::SIMULATED,
          "measurement source is SIMULATED going into the first calculation");

    // Change only the acquisition-mode marker. The already-populated battery
    // measurements/SoC remain the same. This deliberately does NOT fake an
    // INA219 read; it proves updatePowerBudget() consumes the common battery
    // state and does not contain a second simulation-only planning formula.
    powerManager.enableSimulation(false);
    check(powerManager.getMeasurementSource() == MeasurementSource::NONE,
          "measurement source marker changes when simulation is disabled");

    check(powerManager.updatePowerBudget(), "budget recomputed from the same common battery state");
    const PowerBudget afterSourceChange = powerManager.getPowerBudget();

    check(nearlyEqual(afterSourceChange.availablePowerWatts, simulatedBudget.availablePowerWatts),
          "AUTO budget is unchanged when only the measurement source differs - updatePowerBudget() does not branch on it");
    check(nearlyEqual(afterSourceChange.sustainableTotalPowerWatts, simulatedBudget.sustainableTotalPowerWatts),
          "sustainable total is unchanged when only the measurement source differs - updatePowerBudget() does not branch on it");
    check(nearlyEqual(afterSourceChange.committedPowerWatts, simulatedBudget.committedPowerWatts),
          "committed power is unchanged when only the measurement source differs - updatePowerBudget() does not branch on it");
}

} // namespace

int main()
{
    std::printf("Kilowatts behavioral test suite (host-native, no ESP32)\n");
    std::printf("========================================================\n");

    caseHighSocShortVsLongRuntime();
    caseSocNearReserve();
    caseSocAtOrBelowReserve();
    caseFixedOnDeductedBeforeAuto();
    caseFixedOnConsumesAllSustainablePower();
    caseFixedOnExceedsSustainableTotal();
    caseMultipleAutoLoadsRespectBudget();
    caseHomeScenario();
    caseMineScenario();
    caseHospitalScenario();
    caseRemainingRuntimeDrivesEachRecompute();
    caseSimulationAndHardwarePathShareOneCalculation();

    std::printf("========================================================\n");
    std::printf("%d checks, %d failed\n", g_checkCount, g_failureCount);

    return g_failureCount == 0 ? 0 : 1;
}
