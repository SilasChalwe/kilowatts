/**
 * @file test_best_first_search.cpp
 * @brief Host-native correctness tests for BestFirstSearch and its
 *        immediate inputs.
 */

#include "BestFirstSearch.h"
#include "Load.h"
#include "LoadFilter.h"
#include "PowerManager.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

using kilowatts::BestFirstSearch;
using kilowatts::Load;
using kilowatts::LoadFilter;
using kilowatts::LoadMode;
using kilowatts::LoadPower;
using kilowatts::SafePowerLimitCalculator;

namespace {

std::size_t passedChecks = 0U;
std::size_t failedChecks = 0U;

bool nearValue(float actual, float expected, float tolerance = 0.0005F) {
    return std::fabs(actual - expected) <= tolerance;
}

bool reportCheck(const char* name, bool passed) {
    std::printf("%-76s %s\n", name, passed ? "PASS" : "FAIL");
    if (passed) {
        ++passedChecks;
    } else {
        ++failedChecks;
    }
    return passed;
}

void printSection(const char* title) {
    std::printf("\n======================================================================\n");
    std::printf("%s\n", title);
    std::printf("======================================================================\n");
}

BestFirstSearch::Weights makeWeights(float running, float startup, float battery, float priority, float schedule, std::uint16_t maxPriority) {
    BestFirstSearch::Weights w{};
    w.runningPowerWeight = running;
    w.startupPowerWeight = startup;
    w.batteryStressWeight = battery;
    w.priorityWeight = priority;
    w.scheduleWeight = schedule;
    w.maximumAllowedPriority = maxPriority;
    return w;
}

BestFirstSearch::ElectricalPlanningState makePlanningState(
    float soc,
    float socMin,
    float socWarn,
    float vb,
    float maxBatteryW,
    float maxMainA,
    float beforeFixedW,
    float passedToBfsW,
    float fixedOnW)
{
    BestFirstSearch::ElectricalPlanningState s{};
    s.stateOfChargePercent = soc;
    s.minimumStateOfChargePercent = socMin;
    s.warningStateOfChargePercent = socWarn;
    s.batteryBusVoltageVolts = vb;
    s.maximumBatteryPowerWatts = maxBatteryW;
    s.maximumMainCurrentAmps = maxMainA;
    s.powerBeforeFixedLoadsWatts = beforeFixedW;
    s.powerPassedToBestFirstWatts = passedToBfsW;
    s.fixedOnLoadPowerWatts = fixedOnW;
    return s;
}

bool configureEqualWeights(BestFirstSearch& search, std::uint16_t maxPriority = 10U) {
    return search.setSearchScoreWeights(makeWeights(1.0F, 1.0F, 1.0F, 1.0F, 1.0F, maxPriority));
}

const Load::MacAddress MAC = {0x02, 0x00, 0x00, 0x00, 0x00, 0x31};

void testValidationAndScoring() {
    printSection("VALIDATION AND SCORING");

    BestFirstSearch weightChecks;
    reportCheck("Negative weights are rejected",
                !weightChecks.setSearchScoreWeights(makeWeights(-1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 10U)));
    reportCheck("Zero-valued weights are accepted",
                weightChecks.setSearchScoreWeights(makeWeights(0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 10U)));

    BestFirstSearch scoring;
    configureEqualWeights(scoring);
    reportCheck("startSearch() accepts a valid planning state",
                scoring.startSearch(makePlanningState(30.0F, 20.0F, 40.0F, 10.0F, 200.0F, 20.0F, 100.0F, 100.0F, 0.0F)));
    reportCheck("Battery stress term is computed and clamped",
                nearValue(scoring.getBatteryStressTerm(), 0.5F));

    const Load load101(Load::Id{MAC, 16U}, "Load101", LoadPower{20.0F, 30.0F}, 10U, LoadMode::Auto::ON);
    const Load load102(Load::Id{MAC, 17U}, "Load102", LoadPower{10.0F, 10.0F}, 5U, LoadMode::Auto::ON);
    const Load load103(Load::Id{MAC, 18U}, "Load103", LoadPower{5.0F, 5.0F}, 10U, LoadMode::Auto::OFF);

    reportCheck("addLoad() accepts a valid load with schedule penalty 0", scoring.addLoad(load101, 0.0F));
    reportCheck("addLoad() accepts a valid load with schedule penalty 1", scoring.addLoad(load102, 1.0F));
    reportCheck("addLoad() accepts AUTO_OFF as a candidate too", scoring.addLoad(load103, 0.0F));
    reportCheck("run() completes", scoring.run());

    reportCheck("Load101 running ratio is 0.20", nearValue(scoring.getLoadRunningPowerRatio(0U), 0.20F));
    reportCheck("Load102 heuristic cost includes future schedule penalty", nearValue(scoring.getLoadHeuristicCost(1U), 1.50F));
    reportCheck("Load103 final score is 0.55", nearValue(scoring.getLoadFinalScore(2U), 0.55F));
    reportCheck("All three loads are selected",
                scoring.isLoadSelectedToBeOn(0U) && scoring.isLoadSelectedToBeOn(1U) && scoring.isLoadSelectedToBeOn(2U));
    reportCheck("Selected power totals 35 W", nearValue(scoring.getSelectedAutoLoadPowerWatts(), 35.0F));
    reportCheck("Planned ON power totals 35 W", nearValue(scoring.getPlannedOnPowerWatts(), 35.0F));
    reportCheck("Final remaining power is 65 W", nearValue(scoring.getFinalRemainingPowerWatts(), 65.0F));
}

void testConstraintRejectionReasons() {
    printSection("CONSTRAINT REJECTION REASONS");

    {
        BestFirstSearch search;
        configureEqualWeights(search);
        search.startSearch(makePlanningState(20.0F, 20.0F, 40.0F, 10.0F, 100.0F, 10.0F, 0.0F, 0.0F, 0.0F));
        search.addLoad(Load(Load::Id{MAC, 1U}, "LowBattery", LoadPower{1.0F, 1.0F}, 1U, LoadMode::Auto::ON), 0.0F);
        search.run();
        reportCheck("LOW_BATTERY is reported first", search.getLoadSelectionRejectionReason(0U) == BestFirstSearch::LOW_BATTERY);
    }
    {
        BestFirstSearch search;
        configureEqualWeights(search);
        search.startSearch(makePlanningState(80.0F, 20.0F, 40.0F, 10.0F, 100.0F, 10.0F, 100.0F, 5.0F, 0.0F));
        search.addLoad(Load(Load::Id{MAC, 2U}, "PowerLimit", LoadPower{10.0F, 10.0F}, 1U, LoadMode::Auto::ON), 0.0F);
        search.run();
        reportCheck("POWER_LIMIT_EXCEEDED is reported", search.getLoadSelectionRejectionReason(0U) == BestFirstSearch::POWER_LIMIT_EXCEEDED);
    }
    {
        BestFirstSearch search;
        configureEqualWeights(search);
        search.startSearch(makePlanningState(80.0F, 20.0F, 40.0F, 10.0F, 25.0F, 10.0F, 100.0F, 100.0F, 20.0F));
        search.addLoad(Load(Load::Id{MAC, 3U}, "BatteryLimit", LoadPower{1.0F, 10.0F}, 1U, LoadMode::Auto::ON), 0.0F);
        search.run();
        reportCheck("BATTERY_CURRENT_LIMIT is reported", search.getLoadSelectionRejectionReason(0U) == BestFirstSearch::BATTERY_CURRENT_LIMIT);
    }
    {
        BestFirstSearch search;
        configureEqualWeights(search);
        search.startSearch(makePlanningState(80.0F, 20.0F, 40.0F, 10.0F, 100.0F, 1.0F, 100.0F, 100.0F, 0.0F));
        search.addLoad(Load(Load::Id{MAC, 4U}, "MainLimit", LoadPower{1.0F, 20.0F}, 1U, LoadMode::Auto::ON), 0.0F);
        search.run();
        reportCheck("MAIN_LIMIT_EXCEEDED is reported", search.getLoadSelectionRejectionReason(0U) == BestFirstSearch::MAIN_LIMIT_EXCEEDED);
    }
}

void testSequentialAllocation() {
    printSection("SEQUENTIAL ALLOCATION");

    BestFirstSearch search;
    search.setSearchScoreWeights(makeWeights(1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 10U));
    search.startSearch(makePlanningState(90.0F, 10.0F, 20.0F, 10.0F, 1000.0F, 1000.0F, 15.0F, 15.0F, 0.0F));

    const Load loadB(Load::Id{MAC, 16U}, "LoadB", LoadPower{8.0F, 8.0F}, 1U, LoadMode::Auto::ON);
    const Load loadA(Load::Id{MAC, 17U}, "LoadA", LoadPower{10.0F, 10.0F}, 1U, LoadMode::Auto::ON);
    search.addLoad(loadB, 0.0F);
    search.addLoad(loadA, 0.0F);
    search.run();

    reportCheck("Cheaper load is selected first", search.isLoadSelectedToBeOn(0U));
    reportCheck("More expensive load is rejected for power", !search.isLoadSelectedToBeOn(1U) &&
                search.getLoadSelectionRejectionReason(1U) == BestFirstSearch::POWER_LIMIT_EXCEEDED);
    reportCheck("Final remaining power is 7 W", nearValue(search.getFinalRemainingPowerWatts(), 7.0F));
}

void testStaticFeasibilityCheck() {
    printSection("STATIC FEASIBILITY CHECK");

    auto makeFeasible = []() {
        BestFirstSearch::FeasibilityInputs inputs{};
        inputs.stateOfChargePercent = 80.0F;
        inputs.minimumStateOfChargePercent = 20.0F;
        inputs.candidateRunningPowerWatts = 5.0F;
        inputs.candidatePeakPowerWatts = 8.0F;
        inputs.remainingPowerWatts = 50.0F;
        inputs.plannedOnPowerWatts = 10.0F;
        inputs.maximumBatteryPowerWatts = 200.0F;
        inputs.batteryBusVoltageVolts = 12.0F;
        inputs.maximumMainCurrentAmps = 50.0F;
        return inputs;
    };

    reportCheck("A feasible snapshot returns NONE",
                BestFirstSearch::checkFeasibility(makeFeasible()) == BestFirstSearch::NONE);

    {
        auto inputs = makeFeasible();
        inputs.stateOfChargePercent = 20.0F;
        reportCheck("SoC <= minimum yields LOW_BATTERY",
                    BestFirstSearch::checkFeasibility(inputs) == BestFirstSearch::LOW_BATTERY);
    }
    {
        auto inputs = makeFeasible();
        inputs.candidateRunningPowerWatts = 60.0F;
        reportCheck("Running power above remaining yields POWER_LIMIT_EXCEEDED",
                    BestFirstSearch::checkFeasibility(inputs) == BestFirstSearch::POWER_LIMIT_EXCEEDED);
    }
    {
        auto inputs = makeFeasible();
        inputs.plannedOnPowerWatts = 195.0F;
        reportCheck("Battery power limit yields BATTERY_CURRENT_LIMIT",
                    BestFirstSearch::checkFeasibility(inputs) == BestFirstSearch::BATTERY_CURRENT_LIMIT);
    }
    {
        auto inputs = makeFeasible();
        inputs.maximumMainCurrentAmps = 1.0F;
        reportCheck("Main current limit yields MAIN_LIMIT_EXCEEDED",
                    BestFirstSearch::checkFeasibility(inputs) == BestFirstSearch::MAIN_LIMIT_EXCEEDED);
    }
}

struct BruteResult {
    float totalRunningWatts;
    unsigned mask;
};

BruteResult findExactBestSubset(const std::vector<Load>& loads, float remainingPowerWatts, float maxBatteryWatts, float busVolts, float maxMainAmps) {
    BruteResult best{0.0F, 0U};
    const unsigned count = static_cast<unsigned>(loads.size());
    for (unsigned mask = 0U; mask < (1U << count); ++mask) {
        float running = 0.0F;
        float committed = 0.0F;
        bool feasible = true;
        for (unsigned i = 0U; i < count; ++i) {
            if ((mask & (1U << i)) == 0U) continue;
            const LoadPower power = loads[i].getPower();
            if (power.runningWatts > remainingPowerWatts - running) { feasible = false; break; }
            if (committed + power.startupWatts > maxBatteryWatts) { feasible = false; break; }
            if ((committed + power.startupWatts) / busVolts > maxMainAmps) { feasible = false; break; }
            running += power.runningWatts;
            committed += power.runningWatts;
        }
        if (feasible && running > best.totalRunningWatts) {
            best = {running, mask};
        }
    }
    return best;
}

void testExactOptimalityOnSmallSet() {
    printSection("EXACT OPTIMALITY ON SMALL SET");

    std::vector<Load> loads;
    loads.emplace_back(Load::Id{MAC, 1U}, "A", LoadPower{12.0F, 12.0F}, 7U, LoadMode::Auto::ON);
    loads.emplace_back(Load::Id{MAC, 2U}, "B", LoadPower{9.0F, 9.0F}, 8U, LoadMode::Auto::OFF);
    loads.emplace_back(Load::Id{MAC, 3U}, "C", LoadPower{7.0F, 7.0F}, 9U, LoadMode::Auto::ON);
    loads.emplace_back(Load::Id{MAC, 4U}, "D", LoadPower{4.0F, 4.0F}, 10U, LoadMode::Auto::ON);

    const float remainingPowerWatts = 20.0F;
    const float maxBatteryWatts = 40.0F;
    const float busVolts = 12.0F;
    const float maxMainAmps = 5.0F;

    BestFirstSearch search;
    configureEqualWeights(search);
    search.startSearch(makePlanningState(80.0F, 20.0F, 40.0F, busVolts, maxBatteryWatts, maxMainAmps, remainingPowerWatts, remainingPowerWatts, 0.0F));
    for (const Load& load : loads) {
        search.addLoad(load, 0.0F);
    }
    search.run();

    const BruteResult exact = findExactBestSubset(loads, remainingPowerWatts, maxBatteryWatts, busVolts, maxMainAmps);
    float greedyRunning = 0.0F;
    unsigned greedyMask = 0U;
    for (std::size_t i = 0U; i < loads.size(); ++i) {
        if (search.isLoadSelectedToBeOn(i)) {
            greedyRunning += loads[i].getPower().runningWatts;
            greedyMask |= (1U << i);
        }
    }

    reportCheck("Greedy result matches the exact best feasible running-power total",
                nearValue(greedyRunning, exact.totalRunningWatts));
    reportCheck("Greedy result selects the exact best subset for this controlled case",
                greedyMask == exact.mask);
}

void testIntegrationWithLoadFilterAndSafePowerLimit() {
    printSection("INTEGRATION WITH LOADFILTER AND SAFE POWER LIMIT");

    Load fixedOn(Load::Id{MAC, 10U}, "FixedOn", LoadPower{2.0F, 2.0F}, 1U, LoadMode::Fixed::ON);
    Load autoOn(Load::Id{MAC, 11U}, "AutoOn", LoadPower{6.0F, 9.0F}, 2U, LoadMode::Auto::ON);
    Load autoOff(Load::Id{MAC, 12U}, "AutoOff", LoadPower{5.0F, 8.0F}, 3U, LoadMode::Auto::OFF);

    LoadFilter filter;
    filter.addLoad(fixedOn);
    filter.addLoad(autoOn);
    filter.addLoad(autoOff);

    SafePowerLimitCalculator calc;
    SafePowerLimitCalculator::Inputs inputs{};
    inputs.stateOfChargePercent = 80.0F;
    inputs.minimumStateOfChargePercent = 20.0F;
    inputs.nominalBatteryVoltageVolts = 12.0F;
    inputs.batteryCapacityAmpHours = 100.0F;
    inputs.targetRuntimeHours = 4.0F;
    inputs.batteryBusVoltageVolts = 12.0F;
    inputs.maximumBatteryDischargeCurrentAmps = 40.0F;
    inputs.maximumMainCurrentAmps = 30.0F;
    inputs.safetyFactor = 0.9F;
    reportCheck("SafePowerLimitCalculator accepts the test inputs", calc.calculate(inputs));

    BestFirstSearch search;
    configureEqualWeights(search);
    reportCheck("Search starts from safe-power outputs",
                search.startSearch(makePlanningState(80.0F, 20.0F, 40.0F, 12.0F,
                                                    calc.getMaximumBatteryPowerWatts(),
                                                    inputs.maximumMainCurrentAmps,
                                                    calc.getAvailablePowerWatts(),
                                                    std::max(0.0F, calc.getAvailablePowerWatts() - 2.0F),
                                                    2.0F)));

    reportCheck("Fixed ON load is not a candidate", filter.getNumberOfAutoCandidateLoads() == 2U);
    reportCheck("AUTO_ON and AUTO_OFF are both candidates",
                filter.getAutoCandidateLoad(0U) != nullptr && filter.getAutoCandidateLoad(1U) != nullptr);
    reportCheck("Both auto candidates are accepted", search.addLoad(autoOn, 0.0F) && search.addLoad(autoOff, 0.0F));
    reportCheck("Search runs", search.run());
}

struct RealScenarioLoad {
    unsigned nodeIndex;
    Load::MacAddress nodeMac;
    std::uint8_t relayPin;
    const char* name;
    float runningWatts;
    float startupWatts;
    std::uint16_t priority;
    LoadMode::Value mode;
    float schedulePenalty;
};

std::vector<RealScenarioLoad> buildRealScenarioLoads() {
    const Load::MacAddress nodeA = {0x1C, 0xDB, 0xD4, 0x78, 0xE7, 0xB8};
    const Load::MacAddress nodeB = {0x1C, 0xDB, 0xD4, 0x78, 0xE7, 0xC2};
    const Load::MacAddress nodeC = {0x1C, 0xDB, 0xD4, 0x78, 0xE7, 0xD3};
    const Load::MacAddress nodeD = {0x1C, 0xDB, 0xD4, 0x78, 0xE7, 0xE4};

    return {
        {0U, nodeA, 16U, "Node A Fridge", 60.0F, 150.0F, 1U, LoadMode::Fixed::ON, 0.0F},
        {0U, nodeA, 17U, "Node A Lights", 18.0F, 18.0F, 4U, LoadMode::Auto::ON, 0.0F},
        {0U, nodeA, 18U, "Node A TV", 90.0F, 100.0F, 6U, LoadMode::Auto::ON, 0.0F},
        {0U, nodeA, 19U, "Node A Kettle", 800.0F, 900.0F, 9U, LoadMode::Auto::OFF, 0.0F},
        {1U, nodeB, 22U, "Node B Lights", 12.0F, 12.0F, 4U, LoadMode::Auto::ON, 0.0F},
        {1U, nodeB, 23U, "Node B AC", 350.0F, 450.0F, 8U, LoadMode::Auto::ON, 0.0F},
        {1U, nodeB, 24U, "Node B Garage", 20.0F, 20.0F, 5U, LoadMode::Auto::ON, 0.0F},
        {1U, nodeB, 25U, "Node B Heater", 500.0F, 550.0F, 10U, LoadMode::Auto::OFF, 0.8F},
        {2U, nodeC, 28U, "Node C Pump", 90.0F, 200.0F, 1U, LoadMode::Fixed::ON, 0.0F},
        {2U, nodeC, 29U, "Node C Washer", 400.0F, 600.0F, 7U, LoadMode::Auto::OFF, 0.0F},
        {2U, nodeC, 30U, "Node C Pool", 250.0F, 300.0F, 6U, LoadMode::Auto::ON, 0.0F},
        {2U, nodeC, 31U, "Node C Lights", 15.0F, 15.0F, 3U, LoadMode::Auto::ON, 0.0F},
        {3U, nodeD, 34U, "Node D Router", 10.0F, 10.0F, 2U, LoadMode::Fixed::ON, 0.0F},
        {3U, nodeD, 35U, "Node D Fans", 30.0F, 45.0F, 3U, LoadMode::Auto::ON, 0.0F},
        {3U, nodeD, 36U, "Node D Charger", 250.0F, 300.0F, 7U, LoadMode::Auto::OFF, 0.5F},
        {3U, nodeD, 37U, "Node D EV", 1500.0F, 1500.0F, 10U, LoadMode::Auto::OFF, 0.0F}
    };
}

float configuredDemandWatts(const std::vector<RealScenarioLoad>& specs) {
    float demand = 0.0F;
    for (const RealScenarioLoad& spec : specs) {
        if (spec.mode == LoadMode::Fixed::ON || spec.mode == LoadMode::Auto::ON) {
            demand += spec.runningWatts;
        }
    }
    return demand;
}

float runRealScenario(const std::vector<RealScenarioLoad>& specs, float capacityAh, float soc) {
    constexpr float minimumSoc = 20.0F;
    constexpr float warningSoc = 40.0F;
    constexpr float busVolts = 12.0F;
    constexpr float targetRuntimeHours = 4.0F;
    constexpr float maximumBatteryCurrent = 40.0F;
    constexpr float maximumMainCurrent = 30.0F;
    constexpr float safetyFactor = 0.9F;

    SafePowerLimitCalculator::Inputs inputs{};
    inputs.stateOfChargePercent = soc;
    inputs.minimumStateOfChargePercent = minimumSoc;
    inputs.nominalBatteryVoltageVolts = busVolts;
    inputs.batteryCapacityAmpHours = capacityAh;
    inputs.targetRuntimeHours = targetRuntimeHours;
    inputs.batteryBusVoltageVolts = busVolts;
    inputs.maximumBatteryDischargeCurrentAmps = maximumBatteryCurrent;
    inputs.maximumMainCurrentAmps = maximumMainCurrent;
    inputs.safetyFactor = safetyFactor;

    SafePowerLimitCalculator limit;
    const bool limitAccepted = limit.calculate(inputs);
    reportCheck("Real scenario safe-power inputs are accepted", limitAccepted);
    if (!limitAccepted) return 0.0F;

    std::vector<Load> loads;
    loads.reserve(specs.size());
    for (const RealScenarioLoad& spec : specs) {
        loads.emplace_back(Load::Id{spec.nodeMac, spec.relayPin}, spec.name,
                           LoadPower{spec.runningWatts, spec.startupWatts}, spec.priority, spec.mode);
    }

    LoadFilter filter;
    bool allLoadsAccepted = true;
    float fixedOnWatts = 0.0F;
    for (std::size_t i = 0U; i < loads.size(); ++i) {
        allLoadsAccepted = filter.addLoad(loads[i]) && allLoadsAccepted;
        if (specs[i].mode == LoadMode::Fixed::ON) fixedOnWatts += specs[i].runningWatts;
    }
    reportCheck("Real scenario accepts all 16 loads", allLoadsAccepted && loads.size() == 16U);
    reportCheck("Real scenario has four loads on each Node A-D",
                specs.size() == 16U && filter.getNumberOfFixedOnLoads() == 3U &&
                filter.getNumberOfAutoCandidateLoads() == 13U);

    const float powerForAutoLoads = std::max(0.0F, limit.getAvailablePowerWatts() - fixedOnWatts);
    BestFirstSearch search;
    configureEqualWeights(search);
    const bool started = search.startSearch(makePlanningState(
        soc, minimumSoc, warningSoc, busVolts, limit.getMaximumBatteryPowerWatts(), maximumMainCurrent,
        limit.getAvailablePowerWatts(), powerForAutoLoads, fixedOnWatts));
    reportCheck("Best-First starts for this battery scenario", started);
    if (!started) return fixedOnWatts;

    std::vector<std::size_t> candidateSpecIndices;
    for (std::size_t i = 0U; i < filter.getNumberOfAutoCandidateLoads(); ++i) {
        const Load* candidate = filter.getAutoCandidateLoad(i);
        std::size_t specIndex = specs.size();
        for (std::size_t j = 0U; j < loads.size(); ++j) {
            if (&loads[j] == candidate) {
                specIndex = j;
                break;
            }
        }
        const bool added = specIndex < specs.size() &&
                           search.addLoad(*candidate, specs[specIndex].schedulePenalty);
        if (added) candidateSpecIndices.push_back(specIndex);
    }
    reportCheck("All 13 Auto loads reach Best-First", candidateSpecIndices.size() == 13U);
    reportCheck("Real scenario search completes", search.run());

    float admittedAutoWatts = 0.0F;
    for (std::size_t i = 0U; i < candidateSpecIndices.size(); ++i) {
        if (search.isLoadSelectedToBeOn(i)) admittedAutoWatts += specs[candidateSpecIndices[i]].runningWatts;
    }
    const float committedWatts = fixedOnWatts + admittedAutoWatts;
    reportCheck("Admitted Auto power stays within its safe power budget",
                admittedAutoWatts <= powerForAutoLoads + 0.001F);
    reportCheck("Best-First committed power matches the selected loads",
                nearValue(search.getPlannedOnPowerWatts(), committedWatts, 0.01F));

    const float naiveWatts = configuredDemandWatts(specs);
    const float usableEnergyWh = limit.getUsableEnergyWattHours();
    const float coordinatedRuntime = committedWatts > 0.0F ? usableEnergyWh / committedWatts : 0.0F;
    const float naiveRuntime = naiveWatts > 0.0F ? usableEnergyWh / naiveWatts : 0.0F;
    reportCheck("Runtime calculation is finite and based on battery energy",
                std::isfinite(coordinatedRuntime) && std::isfinite(naiveRuntime));
    std::printf("  battery=%5.0f Ah  SoC=%3.0f%%  available=%7.1f W  naive=%7.1f W  coordinated=%7.1f W  runtime=%.2f/%.2f h\n",
                capacityAh, soc, limit.getAvailablePowerWatts(), naiveWatts, committedWatts,
                coordinatedRuntime, naiveRuntime);
    return committedWatts;
}

void testRealFourNodeBatteryScenarios() {
    printSection("REAL SCENARIO: FOUR NODES x FOUR LOADS, BATTERY CAPACITY x SOC");

    const std::vector<RealScenarioLoad> specs = buildRealScenarioLoads();
    const float capacities[] = {20.0F, 100.0F, 1000.0F};
    const float statesOfCharge[] = {25.0F, 50.0F, 75.0F, 100.0F};
    float highestSocCommitted[3] = {0.0F, 0.0F, 0.0F};

    reportCheck("Real installation contains 16 loads", specs.size() == 16U);
    for (std::size_t capacityIndex = 0U; capacityIndex < 3U; ++capacityIndex) {
        float previousSocCommitted = -1.0F;
        for (float soc : statesOfCharge) {
            const float committed = runRealScenario(specs, capacities[capacityIndex], soc);
            reportCheck("Higher SoC does not reduce coordinated load power",
                        previousSocCommitted < 0.0F || committed >= previousSocCommitted - 0.01F);
            previousSocCommitted = committed;
        }
        highestSocCommitted[capacityIndex] = previousSocCommitted;
    }
    reportCheck("Higher battery capacity does not reduce coordinated load power",
                highestSocCommitted[1] >= highestSocCommitted[0] - 0.01F &&
                highestSocCommitted[2] >= highestSocCommitted[1] - 0.01F);
}

} // namespace

int main() {
    std::printf("KILOWATTS BEST-FIRST SEARCH HOST TEST REPORT\n");

    testValidationAndScoring();
    testConstraintRejectionReasons();
    testSequentialAllocation();
    testStaticFeasibilityCheck();
    testExactOptimalityOnSmallSet();
    testIntegrationWithLoadFilterAndSafePowerLimit();
    testRealFourNodeBatteryScenarios();

    printSection("FINAL TEST SUMMARY");
    std::printf("Passed checks: %zu\n", passedChecks);
    std::printf("Failed checks: %zu\n", failedChecks);
    std::printf("OVERALL RESULT: %s\n", failedChecks == 0U ? "PASS" : "FAIL");
    return failedChecks == 0U ? 0 : 1;
}
