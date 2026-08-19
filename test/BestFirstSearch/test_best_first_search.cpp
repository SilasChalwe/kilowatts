/**
 * @file test_best_first_search.cpp
 * @brief Host-native correctness tests for Load, LoadFilter,
 *        AvailablePowerManager and BestFirstSearch.
 *
 * Every expected numeric value in the scoring/rejection sections below is
 * hand-derived from BestFirstSearch.cpp's scoring/admission formulas and
 * cross-checked against its actual implementation.
 *
 * LoadScheduleEvaluator is not exercised here: it calls
 * CurrentTimeProvider's real methods, which require ESP-IDF's SNTP/NVS
 * headers and cannot be linked into a host build (see
 * LoadScheduleEvaluator's own README). Instead, the schedule tests below
 * supply r_i directly to BestFirstSearch::addLoad(), exactly as
 * LoadScheduleEvaluator::evaluateSchedule() would have computed it for
 * each of its three cases (no schedule, schedule not yet due, schedule
 * due) — this still exercises exactly the part of the formula
 * BestFirstSearch is responsible for.
 *
 * This file uses a standard host int main(), not an ESP-IDF app_main(), so
 * it can be compiled and run by run_cpp_test.sh's plain g++ invocation.
 */

#include "BestFirstSearch.h"
#include "Load.h"
#include "LoadFilter.h"
#include "PowerManager.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

using kilowatts::AvailablePowerManager;
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

using BranchId = BestFirstSearch::BranchId;

/*
 * A Branch is physically identified by {Node MAC, relay pin} — the same
 * addressing as Load::Id, since a Branch is the relay-controlled circuit
 * feeding a Load. Two distinct demonstration Nodes give every test below
 * real, distinguishable Branch identities rather than opaque integers.
 */
const Load::MacAddress BRANCH_NODE_A_MAC = {0x02, 0x00, 0x00, 0x00, 0xB0, 0x0A};
const Load::MacAddress BRANCH_NODE_B_MAC = {0x02, 0x00, 0x00, 0x00, 0xB0, 0x0B};

const BranchId BRANCH_0 = BranchId{BRANCH_NODE_A_MAC, 16U};
const BranchId BRANCH_1 = BranchId{BRANCH_NODE_B_MAC, 16U};
const BranchId UNREGISTERED_BRANCH = BranchId{BRANCH_NODE_A_MAC, 99U};

BestFirstSearch::Weights makeWeights(
    float runningPowerWeight,
    float startupPowerWeight,
    float batteryStressWeight,
    float priorityWeight,
    float scheduleWeight,
    std::uint16_t maximumAllowedPriority)
{
    BestFirstSearch::Weights weights{};
    weights.runningPowerWeight = runningPowerWeight;
    weights.startupPowerWeight = startupPowerWeight;
    weights.batteryStressWeight = batteryStressWeight;
    weights.priorityWeight = priorityWeight;
    weights.scheduleWeight = scheduleWeight;
    weights.maximumAllowedPriority = maximumAllowedPriority;
    return weights;
}

BestFirstSearch::ElectricalPlanningState makePlanningState(
    float stateOfChargePercent,
    float minimumStateOfChargePercent,
    float warningStateOfChargePercent,
    float batteryBusVoltageVolts,
    float maximumBatteryPowerWatts,
    float maximumMainCurrentAmps,
    float totalAvailablePowerWatts,
    float powerAvailableForAutoLoadsWatts,
    float initialCommittedPowerWatts)
{
    BestFirstSearch::ElectricalPlanningState state{};
    state.stateOfChargePercent = stateOfChargePercent;
    state.minimumStateOfChargePercent = minimumStateOfChargePercent;
    state.warningStateOfChargePercent = warningStateOfChargePercent;
    state.batteryBusVoltageVolts = batteryBusVoltageVolts;
    state.maximumBatteryPowerWatts = maximumBatteryPowerWatts;
    state.maximumMainCurrentAmps = maximumMainCurrentAmps;
    state.totalAvailablePowerWatts = totalAvailablePowerWatts;
    state.powerAvailableForAutoLoadsWatts = powerAvailableForAutoLoadsWatts;
    state.initialCommittedPowerWatts = initialCommittedPowerWatts;
    return state;
}

bool configureEqualWeights(BestFirstSearch& search, std::uint16_t maximumAllowedPriority = 10U) {
    return search.setSearchScoreWeights(makeWeights(1.0F, 1.0F, 1.0F, 1.0F, 1.0F, maximumAllowedPriority));
}

/**
 * TEST 1 - CURRENT LOAD MODEL
 * Construction with a real six-byte MAC-style Node identity, and the exact
 * acceptance rules implemented by setPower(), setMeasurements() and
 * setSchedule(). Load.h/Load.cpp are unchanged by the Chapter 4 rework, so
 * this test documents the same current behaviour as before.
 */
void testLoadDataModel() {
    printSection("TEST 1 - CURRENT LOAD MODEL");

    const Load::MacAddress sittingRoomMac = {0x1C, 0xDB, 0xD4, 0x78, 0xE7, 0xB8};

    Load fan(Load::Id{sittingRoomMac, 18U}, "Fan", LoadPower{18.0F, 25.0F}, 3U, LoadMode::Auto::ON);

    reportCheck("Load identity is Node MAC address + relay pin, not an integer ID",
                fan.getId().macAddress == sittingRoomMac && fan.getId().relayPin == 18U);
    reportCheck("getMacAddress() matches the Id's MAC address", fan.getMacAddress() == sittingRoomMac);
    reportCheck("getRelayPin() matches the Id's relay pin", fan.getRelayPin() == 18U);
    reportCheck("Constructor stores the supplied name", fan.getName() == "Fan");
    reportCheck("Constructor stores valid power unchanged",
                nearValue(fan.getPower().runningWatts, 18.0F) &&
                nearValue(fan.getPower().startupWatts, 25.0F));
    reportCheck("Constructor stores the supplied priority", fan.getPriority() == 3U);
    reportCheck("Constructor stores the supplied combined mode", fan.getMode() == LoadMode::Auto::ON);
    reportCheck("AUTO_ON Load reports isAuto() and isOn()", fan.isAuto() && fan.isOn());
    reportCheck("AUTO_ON Load does not report isFixed() or isOff()",
                !fan.isFixed() && !fan.isOff());

    fan.setName("Ceiling Fan");
    reportCheck("setName() changes the stored name", fan.getName() == "Ceiling Fan");

    const bool validPowerAccepted = fan.setPower(LoadPower{20.0F, 28.0F});
    reportCheck("setPower() accepts startupWatts >= runningWatts",
                validPowerAccepted && nearValue(fan.getPower().runningWatts, 20.0F) &&
                nearValue(fan.getPower().startupWatts, 28.0F));

    const bool invalidPowerRejected = !fan.setPower(LoadPower{20.0F, 10.0F});
    reportCheck("setPower() rejects startupWatts < runningWatts",
                invalidPowerRejected && nearValue(fan.getPower().runningWatts, 20.0F));

    const bool scheduleAccepted = fan.setSchedule(kilowatts::AutoSchedule{true, 6U, 30U});
    reportCheck("setSchedule() accepts a valid time on an AUTO Load",
                scheduleAccepted && fan.getSchedule().enabled &&
                fan.getSchedule().hour == 6U && fan.getSchedule().minute == 30U);

    fan.setMode(LoadMode::Fixed::ON);
    reportCheck("setMode() to FIXED clears any AUTO schedule",
                fan.isFixed() && !fan.getSchedule().enabled);

    Load router(Load::Id{sittingRoomMac, 17U}, "Router", LoadPower{10.0F, 10.0F}, 2U, LoadMode::Fixed::OFF);
    reportCheck("FIXED_OFF Load reports isFixed() and isOff()",
                router.isFixed() && router.isOff() && !router.isOn());
}

/**
 * TEST 2 - LOADFILTER CLASSIFICATION
 * LoadFilter.h/LoadFilter.cpp are unchanged by this phase: AUTO_ON and
 * AUTO_OFF both remain Auto candidates, Fixed Loads remain boundary
 * conditions.
 */
void testLoadFilterClassification() {
    printSection("TEST 2 - LOADFILTER CLASSIFICATION");

    const Load::MacAddress centralMac = {0xA4, 0xCF, 0x12, 0x0E, 0x32, 0xC0};
    const Load::MacAddress sittingRoomMac = {0x1C, 0xDB, 0xD4, 0x78, 0xE7, 0xB8};

    Load fixedOnLoad(Load::Id{centralMac, 25U}, "Central Status Indicator",
                      LoadPower{2.0F, 2.0F}, 1U, LoadMode::Fixed::ON);
    Load fixedOffLoad(Load::Id{sittingRoomMac, 17U}, "Router",
                       LoadPower{10.0F, 10.0F}, 2U, LoadMode::Fixed::OFF);
    Load autoOnLoad(Load::Id{sittingRoomMac, 18U}, "Fan",
                     LoadPower{18.0F, 25.0F}, 3U, LoadMode::Auto::ON);
    Load autoOffLoad(Load::Id{sittingRoomMac, 19U}, "WaterPump",
                      LoadPower{35.0F, 55.0F}, 4U, LoadMode::Auto::OFF);

    LoadFilter loadFilter;
    reportCheck("addLoad() accepts a FIXED_ON Load", loadFilter.addLoad(fixedOnLoad));
    reportCheck("addLoad() accepts a FIXED_OFF Load", loadFilter.addLoad(fixedOffLoad));
    reportCheck("addLoad() accepts an AUTO_ON Load", loadFilter.addLoad(autoOnLoad));
    reportCheck("addLoad() accepts an AUTO_OFF Load", loadFilter.addLoad(autoOffLoad));

    reportCheck("FIXED_ON routes to the Fixed ON collection",
                loadFilter.getNumberOfFixedOnLoads() == 1U &&
                loadFilter.getFixedOnLoad(0U) == &fixedOnLoad);
    reportCheck("AUTO_ON routes to the Auto candidate collection",
                loadFilter.getNumberOfAutoCandidateLoads() == 2U &&
                loadFilter.getAutoCandidateLoad(0U) == &autoOnLoad);
    reportCheck("AUTO_OFF also routes to the Auto candidate collection",
                loadFilter.getAutoCandidateLoad(1U) == &autoOffLoad);

    loadFilter.reset();
    reportCheck("reset() clears every collection",
                loadFilter.getNumberOfFixedOnLoads() == 0U &&
                loadFilter.getNumberOfFixedOffLoads() == 0U &&
                loadFilter.getNumberOfAutoCandidateLoads() == 0U);
}

/**
 * TEST 3 - SEARCH SCORE WEIGHT VALIDATION
 * setSearchScoreWeights() now takes one BestFirstSearch::Weights value
 * carrying w_P, w_S, w_B, w_Q, w_T and W_max.
 */
void testSearchScoreWeightValidation() {
    printSection("TEST 3 - SEARCH SCORE WEIGHT VALIDATION");

    BestFirstSearch rejectNegative;
    reportCheck("A negative weight is rejected",
                !rejectNegative.setSearchScoreWeights(makeWeights(-1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 10U)));

    BestFirstSearch rejectZeroPriority;
    reportCheck("maximumAllowedPriority == 0 is rejected",
                !rejectZeroPriority.setSearchScoreWeights(makeWeights(1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 0U)));

    BestFirstSearch rejectNonFinite;
    const float notANumber = std::numeric_limits<float>::quiet_NaN();
    reportCheck("A non-finite weight is rejected",
                !rejectNonFinite.setSearchScoreWeights(makeWeights(notANumber, 1.0F, 1.0F, 1.0F, 1.0F, 10U)));

    BestFirstSearch acceptsZeroWeights;
    reportCheck("Zero-valued weights are accepted (zero is non-negative)",
                acceptsZeroWeights.setSearchScoreWeights(makeWeights(0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 10U)));

    BestFirstSearch reconfigureAfterStart;
    reportCheck("Weights configure successfully before a search starts", configureEqualWeights(reconfigureAfterStart));
    reportCheck("startSearch() succeeds once weights are configured",
                reconfigureAfterStart.startSearch(
                    makePlanningState(80.0F, 20.0F, 40.0F, 12.0F, 500.0F, 100.0F, 50.0F, 50.0F, 0.0F)));
    reportCheck("setSearchScoreWeights() is rejected once a search has started",
                !reconfigureAfterStart.setSearchScoreWeights(makeWeights(2.0F, 2.0F, 2.0F, 2.0F, 2.0F, 10U)));
}

/**
 * TEST 4 - ELECTRICAL PLANNING STATE VALIDATION
 * startSearch() rejects an ElectricalPlanningState with an out-of-range
 * percentage, SoC_warn not strictly above SoC_min (Equation 4.29's
 * denominator would be zero or negative), a non-positive battery bus
 * voltage, or a negative power/current figure.
 */
void testElectricalPlanningStateValidation() {
    printSection("TEST 4 - ELECTRICAL PLANNING STATE VALIDATION");

    BestFirstSearch weightsNotConfigured;
    reportCheck("startSearch() is rejected before weights are configured",
                !weightsNotConfigured.startSearch(
                    makePlanningState(80.0F, 20.0F, 40.0F, 12.0F, 500.0F, 100.0F, 50.0F, 50.0F, 0.0F)));

    BestFirstSearch socOutOfRange;
    configureEqualWeights(socOutOfRange);
    reportCheck("SoC above 100 is rejected",
                !socOutOfRange.startSearch(
                    makePlanningState(150.0F, 20.0F, 40.0F, 12.0F, 500.0F, 100.0F, 50.0F, 50.0F, 0.0F)));

    BestFirstSearch warnEqualsMin;
    configureEqualWeights(warnEqualsMin);
    reportCheck("SoC_warn equal to SoC_min is rejected (zero stress denominator)",
                !warnEqualsMin.startSearch(
                    makePlanningState(80.0F, 20.0F, 20.0F, 12.0F, 500.0F, 100.0F, 50.0F, 50.0F, 0.0F)));

    BestFirstSearch warnBelowMin;
    configureEqualWeights(warnBelowMin);
    reportCheck("SoC_warn below SoC_min is rejected (negative stress denominator)",
                !warnBelowMin.startSearch(
                    makePlanningState(80.0F, 30.0F, 20.0F, 12.0F, 500.0F, 100.0F, 50.0F, 50.0F, 0.0F)));

    BestFirstSearch zeroVoltage;
    configureEqualWeights(zeroVoltage);
    reportCheck("A battery bus voltage of zero is rejected",
                !zeroVoltage.startSearch(
                    makePlanningState(80.0F, 20.0F, 40.0F, 0.0F, 500.0F, 100.0F, 50.0F, 50.0F, 0.0F)));

    BestFirstSearch negativeVoltage;
    configureEqualWeights(negativeVoltage);
    reportCheck("A negative battery bus voltage is rejected",
                !negativeVoltage.startSearch(
                    makePlanningState(80.0F, 20.0F, 40.0F, -12.0F, 500.0F, 100.0F, 50.0F, 50.0F, 0.0F)));

    BestFirstSearch negativePower;
    configureEqualWeights(negativePower);
    reportCheck("A negative totalAvailablePowerWatts is rejected",
                !negativePower.startSearch(
                    makePlanningState(80.0F, 20.0F, 40.0F, 12.0F, 500.0F, 100.0F, -1.0F, 50.0F, 0.0F)));

    BestFirstSearch validState;
    configureEqualWeights(validState);
    reportCheck("A fully valid planning state is accepted",
                validState.startSearch(
                    makePlanningState(80.0F, 20.0F, 40.0F, 12.0F, 500.0F, 100.0F, 50.0F, 50.0F, 0.0F)));

    BestFirstSearch inProgress;
    configureEqualWeights(inProgress);
    inProgress.startSearch(makePlanningState(80.0F, 20.0F, 40.0F, 12.0F, 500.0F, 100.0F, 50.0F, 50.0F, 0.0F));
    reportCheck("startSearch() is rejected while a started-but-incomplete search has not been run() yet",
                !inProgress.startSearch(
                    makePlanningState(80.0F, 20.0F, 40.0F, 12.0F, 500.0F, 100.0F, 20.0F, 20.0F, 0.0F)));
}

/**
 * TEST 5 - REGISTERBRANCH VALIDATION
 */
void testRegisterBranchValidation() {
    printSection("TEST 5 - REGISTERBRANCH VALIDATION");

    BestFirstSearch beforeStart;
    configureEqualWeights(beforeStart);
    reportCheck("registerBranch() is rejected before startSearch()",
                !beforeStart.registerBranch(BRANCH_0, 0.0F, 10.0F));

    BestFirstSearch search;
    configureEqualWeights(search);
    search.startSearch(makePlanningState(80.0F, 20.0F, 40.0F, 12.0F, 500.0F, 100.0F, 50.0F, 50.0F, 0.0F));

    reportCheck("registerBranch() rejects a negative initial committed power",
                !search.registerBranch(BRANCH_0, -1.0F, 10.0F));
    reportCheck("registerBranch() rejects a negative maximum current",
                !search.registerBranch(BRANCH_0, 0.0F, -10.0F));

    reportCheck("registerBranch() accepts a valid branch", search.registerBranch(BRANCH_0, 0.0F, 10.0F));
    reportCheck("registerBranch() rejects registering the same BranchId twice",
                !search.registerBranch(BRANCH_0, 5.0F, 20.0F));
    reportCheck("The first registration's values are kept after a rejected duplicate",
                nearValue(search.getBranchCommittedPowerWatts(BRANCH_0), 0.0F));

    reportCheck("A second, distinct BranchId registers independently",
                search.registerBranch(BRANCH_1, 3.0F, 8.0F));
    reportCheck("Each branch's committed power is tracked independently",
                nearValue(search.getBranchCommittedPowerWatts(BRANCH_0), 0.0F) &&
                nearValue(search.getBranchCommittedPowerWatts(BRANCH_1), 3.0F));

    reportCheck("getBranchCommittedPowerWatts() on an unregistered BranchId returns 0",
                nearValue(search.getBranchCommittedPowerWatts(UNREGISTERED_BRANCH), 0.0F));
}

/**
 * TEST 5B - BRANCH IDENTITY IS {NODE MAC, RELAY PIN}
 * A Branch is one relay-controlled circuit on one Node. Two Nodes reusing
 * the same relay pin number must remain distinct Branches, and one Node
 * using two different relay pins must also remain distinct Branches —
 * BranchId must never collapse to just the MAC or just the pin.
 */
void testBranchIdentityIsMacPlusRelayPin() {
    printSection("TEST 5B - BRANCH IDENTITY IS {NODE MAC, RELAY PIN}");

    const Load::MacAddress nodeA = {0x02, 0x00, 0x00, 0x00, 0xC0, 0x0A};
    const Load::MacAddress nodeB = {0x02, 0x00, 0x00, 0x00, 0xC0, 0x0B};

    BestFirstSearch search;
    configureEqualWeights(search);
    search.startSearch(makePlanningState(80.0F, 20.0F, 40.0F, 12.0F, 500.0F, 100.0F, 100.0F, 100.0F, 0.0F));

    const BranchId nodeAPin16 = BranchId{nodeA, 16U};
    const BranchId nodeBPin16 = BranchId{nodeB, 16U};
    const BranchId nodeAPin17 = BranchId{nodeA, 17U};

    reportCheck("Node A / pin 16 registers", search.registerBranch(nodeAPin16, 0.0F, 10.0F));
    reportCheck("Node B / pin 16 (same pin, different Node MAC) registers independently",
                search.registerBranch(nodeBPin16, 0.0F, 10.0F));
    reportCheck("Node A / pin 17 (same Node, different pin) registers independently",
                search.registerBranch(nodeAPin17, 0.0F, 10.0F));

    Load loadOnNodeAPin16(Load::Id{nodeA, 16U}, "NodeA-Pin16", LoadPower{4.0F, 4.0F}, 5U, LoadMode::Auto::ON);
    Load loadOnNodeBPin16(Load::Id{nodeB, 16U}, "NodeB-Pin16", LoadPower{6.0F, 6.0F}, 5U, LoadMode::Auto::ON);

    search.addLoad(loadOnNodeAPin16, nodeAPin16, 0.0F);
    search.addLoad(loadOnNodeBPin16, nodeBPin16, 0.0F);
    reportCheck("Search over the two same-pin-different-Node loads runs to completion", search.run());

    reportCheck("Node A / pin 16 committed power reflects only its own Load (4W)",
                nearValue(search.getBranchCommittedPowerWatts(nodeAPin16), 4.0F));
    reportCheck("Node B / pin 16 committed power reflects only its own Load (6W), proving MAC distinguishes the branches",
                nearValue(search.getBranchCommittedPowerWatts(nodeBPin16), 6.0F));
    reportCheck("Node A / pin 17 stayed at 0W (no Load was added to it)",
                nearValue(search.getBranchCommittedPowerWatts(nodeAPin17), 0.0F));
}

/**
 * TEST 6 - ADDLOAD VALIDATION
 * A search must be started and not completed, branchId must already be
 * registered, scheduleFuturePenalty must lie in [0, 1], running power
 * must be > 0, startup power must be >= running power, and priority must
 * not exceed maximumAllowedPriority.
 */
void testAddLoadValidation() {
    printSection("TEST 6 - ADDLOAD VALIDATION");

    const Load::MacAddress mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x21};
    Load validLoad(Load::Id{mac, 16U}, "Valid", LoadPower{5.0F, 8.0F}, 3U, LoadMode::Auto::ON);

    BestFirstSearch notStarted;
    configureEqualWeights(notStarted);
    reportCheck("addLoad() is rejected before startSearch()", !notStarted.addLoad(validLoad, BRANCH_0, 0.0F));

    BestFirstSearch search;
    configureEqualWeights(search);
    search.startSearch(makePlanningState(80.0F, 20.0F, 40.0F, 12.0F, 500.0F, 100.0F, 100.0F, 100.0F, 0.0F));

    reportCheck("addLoad() rejects an unregistered branchId", !search.addLoad(validLoad, BRANCH_0, 0.0F));

    search.registerBranch(BRANCH_0, 0.0F, 50.0F);

    reportCheck("addLoad() rejects scheduleFuturePenalty below 0",
                !search.addLoad(validLoad, BRANCH_0, -0.1F));
    reportCheck("addLoad() rejects scheduleFuturePenalty above 1",
                !search.addLoad(validLoad, BRANCH_0, 1.1F));

    reportCheck("addLoad() accepts a valid AUTO Load", search.addLoad(validLoad, BRANCH_0, 0.0F));
    reportCheck("addLoad() rejects the same Load object added twice",
                !search.addLoad(validLoad, BRANCH_0, 0.0F));

    Load zeroRunningPower(Load::Id{mac, 17U}, "ZeroRunning", LoadPower{0.0F, 0.0F}, 1U, LoadMode::Auto::OFF);
    reportCheck("addLoad() rejects runningWatts == 0 even though Load itself allows it",
                !search.addLoad(zeroRunningPower, BRANCH_0, 0.0F));

    Load priorityTooHigh(Load::Id{mac, 18U}, "TooHighPriority", LoadPower{5.0F, 5.0F}, 11U, LoadMode::Auto::ON);
    reportCheck("addLoad() rejects priority above maximumAllowedPriority",
                !search.addLoad(priorityTooHigh, BRANCH_0, 0.0F));

    Load priorityAtLimit(Load::Id{mac, 19U}, "AtLimitPriority", LoadPower{5.0F, 5.0F}, 10U, LoadMode::Auto::ON);
    reportCheck("addLoad() accepts priority exactly equal to maximumAllowedPriority",
                search.addLoad(priorityAtLimit, BRANCH_0, 0.0F));

    search.run();
    Load afterCompletion(Load::Id{mac, 20U}, "AfterCompletion", LoadPower{5.0F, 5.0F}, 1U, LoadMode::Auto::ON);
    reportCheck("addLoad() is rejected after run() has completed the search",
                !search.addLoad(afterCompletion, BRANCH_0, 0.0F));
}

/**
 * TEST 7 - CHAPTER 4 WORKED EXAMPLE: CANDIDATE EVALUATION (Algorithm 4.3)
 * SoC=30%, SoC_min=20%, SoC_warn=40%, so B = (40-30)/(40-20) = 0.5.
 * P_available (total) = 100 W, P_remaining (auto) starts at 100 W.
 * All five weights are 1.0, W_max = 10.
 *
 * Load101 (branch 0): P_i=20W P_i_peak=30W W_i=10 r_i=0 (no schedule)
 * Load102 (branch 0): P_i=10W P_i_peak=10W W_i=5  r_i=1 (schedule not due)
 * Load103 (branch 1): P_i=5W  P_i_peak=5W  W_i=10 r_i=0 (no schedule)
 *
 * Hand-derived (Equations 4.27-4.33):
 *   101: p=0.20 s=0.10 q=1.00 g=0.80 h=0.00 f=0.80
 *   102: p=0.10 s=0.00 q=0.50 g=0.60 h=1.50 f=2.10
 *   103: p=0.05 s=0.00 q=1.00 g=0.55 h=0.00 f=0.55
 * Best-First order (smallest f first): 103 -> 101 -> 102.
 * All three fit within 100 W and the generous branch/battery/main limits
 * below, so all three are admitted (Algorithm 4.5):
 *   P_remaining = 100 - 5 - 20 - 10 = 65 W
 *   P_committed = 0 + 5 + 20 + 10 = 35 W
 *   branch 0 committed = 20 + 10 = 30 W (Load101 + Load102)
 *   branch 1 committed = 5 W (Load103)
 */
void testChapter4WorkedExample() {
    printSection("TEST 7 - CHAPTER 4 WORKED EXAMPLE: SCORING, ORDERING AND ADMISSION");

    const Load::MacAddress mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x31};

    BestFirstSearch search;
    reportCheck("Weights configured (wP=wS=wB=wQ=wT=1, Wmax=10)", configureEqualWeights(search));
    reportCheck("Search started: SoC=30 SoC_min=20 SoC_warn=40 Ptotal=100 Pauto=100",
                search.startSearch(makePlanningState(30.0F, 20.0F, 40.0F, 10.0F, 200.0F, 20.0F, 100.0F, 100.0F, 0.0F)));
    reportCheck("Battery stress B = (40-30)/(40-20) = 0.5", nearValue(search.getBatteryStressTerm(), 0.5F));

    reportCheck("Branch 0 registered: committed=0W max=10A", search.registerBranch(BRANCH_0, 0.0F, 10.0F));
    reportCheck("Branch 1 registered: committed=0W max=10A", search.registerBranch(BRANCH_1, 0.0F, 10.0F));

    Load load101(Load::Id{mac, 16U}, "Load101", LoadPower{20.0F, 30.0F}, 10U, LoadMode::Auto::ON);
    Load load102(Load::Id{mac, 17U}, "Load102", LoadPower{10.0F, 10.0F}, 5U, LoadMode::Auto::ON);
    Load load103(Load::Id{mac, 18U}, "Load103", LoadPower{5.0F, 5.0F}, 10U, LoadMode::Auto::OFF);

    reportCheck("Load101 added on branch 0 with r_i=0", search.addLoad(load101, BRANCH_0, 0.0F));
    reportCheck("Load102 added on branch 0 with r_i=1 (schedule not due)", search.addLoad(load102, BRANCH_0, 1.0F));
    reportCheck("Load103 added on branch 1 with r_i=0", search.addLoad(load103, BRANCH_1, 0.0F));

    reportCheck("Search runs to completion", search.run());

    reportCheck("Load101 p_i = 20/100 = 0.20", nearValue(search.getLoadRunningPowerRatio(0U), 0.20F));
    reportCheck("Load101 s_i = (30-20)/100 = 0.10", nearValue(search.getLoadStartupPowerRatio(0U), 0.10F));
    reportCheck("Load101 q_i = 10/10 = 1.00", nearValue(search.getLoadPriorityRatio(0U), 1.00F));
    reportCheck("Load101 g(i) = 0.20+0.10+0.5 = 0.80", nearValue(search.getLoadPhysicalCost(0U), 0.80F));
    reportCheck("Load101 h(i) = (1-1.00)+0 = 0.00", nearValue(search.getLoadHeuristicCost(0U), 0.00F));
    reportCheck("Load101 f(i) = 0.80", nearValue(search.getLoadFinalScore(0U), 0.80F));

    reportCheck("Load102 p_i = 10/100 = 0.10", nearValue(search.getLoadRunningPowerRatio(1U), 0.10F));
    reportCheck("Load102 s_i = (10-10)/100 = 0.00", nearValue(search.getLoadStartupPowerRatio(1U), 0.00F));
    reportCheck("Load102 q_i = 5/10 = 0.50", nearValue(search.getLoadPriorityRatio(1U), 0.50F));
    reportCheck("Load102 g(i) = 0.10+0.00+0.5 = 0.60", nearValue(search.getLoadPhysicalCost(1U), 0.60F));
    reportCheck("Load102 h(i) = (1-0.50)+1 = 1.50", nearValue(search.getLoadHeuristicCost(1U), 1.50F));
    reportCheck("Load102 f(i) = 2.10", nearValue(search.getLoadFinalScore(1U), 2.10F));

    reportCheck("Load103 p_i = 5/100 = 0.05", nearValue(search.getLoadRunningPowerRatio(2U), 0.05F));
    reportCheck("Load103 s_i = (5-5)/100 = 0.00", nearValue(search.getLoadStartupPowerRatio(2U), 0.00F));
    reportCheck("Load103 q_i = 10/10 = 1.00", nearValue(search.getLoadPriorityRatio(2U), 1.00F));
    reportCheck("Load103 g(i) = 0.05+0.00+0.5 = 0.55", nearValue(search.getLoadPhysicalCost(2U), 0.55F));
    reportCheck("Load103 h(i) = (1-1.00)+0 = 0.00", nearValue(search.getLoadHeuristicCost(2U), 0.00F));
    reportCheck("Load103 f(i) = 0.55 (smallest -> extracted first by the min-heap)",
                nearValue(search.getLoadFinalScore(2U), 0.55F));

    reportCheck("All three candidates admitted (0.55 < 0.80 < 2.10 all fit)",
                search.isLoadSelectedToBeOn(0U) && search.isLoadSelectedToBeOn(1U) && search.isLoadSelectedToBeOn(2U));
    reportCheck("All three rejection reasons are NONE",
                search.getLoadSelectionRejectionReason(0U) == BestFirstSearch::NONE &&
                search.getLoadSelectionRejectionReason(1U) == BestFirstSearch::NONE &&
                search.getLoadSelectionRejectionReason(2U) == BestFirstSearch::NONE);

    reportCheck("Final P_remaining = 100 - 5 - 20 - 10 = 65 W", nearValue(search.getRemainingPowerWatts(), 65.0F));
    reportCheck("Final P_committed = 5 + 20 + 10 = 35 W", nearValue(search.getCommittedPowerWatts(), 35.0F));
    reportCheck("Branch 0 committed power = 20 + 10 = 30 W",
                nearValue(search.getBranchCommittedPowerWatts(BRANCH_0), 30.0F));
    reportCheck("Branch 1 committed power = 5 W", nearValue(search.getBranchCommittedPowerWatts(BRANCH_1), 5.0F));
    reportCheck("getTotalAvailablePowerWatts() stays the constant P_available=100W",
                nearValue(search.getTotalAvailablePowerWatts(), 100.0F));

    reportCheck("Load101's stored priority is unchanged after scoring/scheduling", load101.getPriority() == 10U);
    reportCheck("Load102's stored priority is unchanged after scoring/scheduling", load102.getPriority() == 5U);
}

/**
 * TEST 8 - ALL FIVE CONSTRAINT-GUARD REJECTION REASONS (Algorithm 4.4)
 * Each case isolates exactly one failing condition; every other input is
 * kept generous enough to pass. Values are hand-derived directly from
 * Equations 4.20-4.25.
 */
void testConstraintGuardRejectionReasons() {
    printSection("TEST 8 - CONSTRAINT GUARD REJECTION REASONS");

    const Load::MacAddress mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x41};

    {
        // SoC(20) <= SoC_min(20) -> LOW_BATTERY, checked before any other condition.
        BestFirstSearch search;
        configureEqualWeights(search);
        search.startSearch(makePlanningState(20.0F, 20.0F, 40.0F, 10.0F, 100.0F, 10.0F, 0.0F, 0.0F, 0.0F));
        search.registerBranch(BRANCH_0, 0.0F, 10.0F);
        Load load(Load::Id{mac, 1U}, "LowBattery", LoadPower{1.0F, 1.0F}, 1U, LoadMode::Auto::ON);
        search.addLoad(load, BRANCH_0, 0.0F);
        search.run();
        reportCheck("SoC <= SoC_min yields LOW_BATTERY",
                    search.getLoadSelectionRejectionReason(0U) == BestFirstSearch::LOW_BATTERY);
        reportCheck("LOW_BATTERY candidate is not selected", !search.isLoadSelectedToBeOn(0U));
    }

    {
        // P_i(10) > P_remaining(5) -> POWER_LIMIT_EXCEEDED, independent of
        // P_available(total)=100W: this is exactly why P_available and
        // P_remaining must be two separate fields.
        BestFirstSearch search;
        configureEqualWeights(search);
        search.startSearch(makePlanningState(80.0F, 20.0F, 40.0F, 10.0F, 100.0F, 10.0F, 100.0F, 5.0F, 0.0F));
        search.registerBranch(BRANCH_0, 0.0F, 10.0F);
        Load load(Load::Id{mac, 2U}, "HighPowerLoad", LoadPower{10.0F, 10.0F}, 1U, LoadMode::Auto::ON);
        search.addLoad(load, BRANCH_0, 0.0F);
        search.run();
        reportCheck("P_i > P_remaining yields POWER_LIMIT_EXCEEDED",
                    search.getLoadSelectionRejectionReason(0U) == BestFirstSearch::POWER_LIMIT_EXCEEDED);
        reportCheck("P_remaining is unchanged by a rejected candidate", nearValue(search.getRemainingPowerWatts(), 5.0F));
    }

    {
        // P_committed(20) + P_i_peak(10) = 30 > P_battery_max(25) -> BATTERY_CURRENT_LIMIT.
        BestFirstSearch search;
        configureEqualWeights(search);
        search.startSearch(makePlanningState(80.0F, 20.0F, 40.0F, 10.0F, 25.0F, 10.0F, 100.0F, 80.0F, 20.0F));
        search.registerBranch(BRANCH_0, 0.0F, 10.0F);
        Load load(Load::Id{mac, 3U}, "BatteryLimit", LoadPower{1.0F, 10.0F}, 1U, LoadMode::Auto::ON);
        search.addLoad(load, BRANCH_0, 0.0F);
        search.run();
        reportCheck("P_committed + P_i_peak > P_battery,max yields BATTERY_CURRENT_LIMIT",
                    search.getLoadSelectionRejectionReason(0U) == BestFirstSearch::BATTERY_CURRENT_LIMIT);
        reportCheck("P_committed is unchanged by a rejected candidate", nearValue(search.getCommittedPowerWatts(), 20.0F));
    }

    {
        // I_main,start = (0+20)/10 = 2A > I_main,max(1A) -> MAIN_LIMIT_EXCEEDED.
        BestFirstSearch search;
        configureEqualWeights(search);
        search.startSearch(makePlanningState(80.0F, 20.0F, 40.0F, 10.0F, 100.0F, 1.0F, 100.0F, 100.0F, 0.0F));
        search.registerBranch(BRANCH_0, 0.0F, 10.0F);
        Load load(Load::Id{mac, 4U}, "MainLimit", LoadPower{1.0F, 20.0F}, 1U, LoadMode::Auto::ON);
        search.addLoad(load, BRANCH_0, 0.0F);
        search.run();
        reportCheck("I_main,start > I_main,max yields MAIN_LIMIT_EXCEEDED",
                    search.getLoadSelectionRejectionReason(0U) == BestFirstSearch::MAIN_LIMIT_EXCEEDED);
    }

    {
        // I_branch,start = (0+20)/10 = 2A > I_branch,max(1A) -> BRANCH_LIMIT_EXCEEDED.
        BestFirstSearch search;
        configureEqualWeights(search);
        search.startSearch(makePlanningState(80.0F, 20.0F, 40.0F, 10.0F, 100.0F, 10.0F, 100.0F, 100.0F, 0.0F));
        search.registerBranch(BRANCH_0, 0.0F, 1.0F);
        Load load(Load::Id{mac, 5U}, "BranchLimit", LoadPower{1.0F, 20.0F}, 1U, LoadMode::Auto::ON);
        search.addLoad(load, BRANCH_0, 0.0F);
        search.run();
        reportCheck("I_branch,start > I_branch,max yields BRANCH_LIMIT_EXCEEDED",
                    search.getLoadSelectionRejectionReason(0U) == BestFirstSearch::BRANCH_LIMIT_EXCEEDED);
        reportCheck("Branch committed power is unchanged by a rejected candidate",
                    nearValue(search.getBranchCommittedPowerWatts(BRANCH_0), 0.0F));
    }
}

/**
 * TEST 9 - SEQUENTIAL ALLOCATION (Algorithm 4.5)
 * Two Auto candidates on the same branch: the cheaper one (by f(i)) is
 * extracted and admitted first, consuming part of P_remaining before the
 * second candidate is even checked — so the second is rejected purely
 * because the first one already used the power it needed, not because
 * the original limit could never have covered it.
 */
void testSequentialAllocation() {
    printSection("TEST 9 - SEQUENTIAL ALLOCATION OF REMAINING CAPACITY");

    const Load::MacAddress mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x51};

    BestFirstSearch search;
    /* Only running power drives ordering here (all other weights zero). */
    search.setSearchScoreWeights(makeWeights(1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 10U));
    search.startSearch(makePlanningState(90.0F, 10.0F, 20.0F, 10.0F, 1000.0F, 1000.0F, 15.0F, 15.0F, 0.0F));
    search.registerBranch(BRANCH_0, 0.0F, 1000.0F);

    Load loadB(Load::Id{mac, 16U}, "LoadB", LoadPower{8.0F, 8.0F}, 1U, LoadMode::Auto::ON);
    Load loadA(Load::Id{mac, 17U}, "LoadA", LoadPower{10.0F, 10.0F}, 1U, LoadMode::Auto::ON);

    search.addLoad(loadB, BRANCH_0, 0.0F);
    search.addLoad(loadA, BRANCH_0, 0.0F);

    reportCheck("Search runs to completion", search.run());

    /* p(B) = 8/15 = 0.5333 < p(A) = 10/15 = 0.6667, so B is admitted first. */
    reportCheck("The cheaper Load (LoadB) is admitted first", search.isLoadSelectedToBeOn(0U));
    reportCheck("The more expensive Load (LoadA) is rejected with POWER_LIMIT_EXCEEDED",
                !search.isLoadSelectedToBeOn(1U) &&
                search.getLoadSelectionRejectionReason(1U) == BestFirstSearch::POWER_LIMIT_EXCEEDED);
    reportCheck("Remaining power after the cycle is 15 - 8 = 7 W", nearValue(search.getRemainingPowerWatts(), 7.0F));
    reportCheck("Committed power after the cycle is 8 W", nearValue(search.getCommittedPowerWatts(), 8.0F));
}

/**
 * TEST 10 - INCREMENTAL BRANCH CURRENT
 * Three candidates on the same branch, admitted in a strictly deterministic
 * order (tiny priority differences break ties), each with P_i=10W and
 * P_i_peak=20W. V_B=10V, I_branch,max=3A:
 *   Load1: (0+20)/10  = 2A <= 3A -> ON, branch = 10 W
 *   Load2: (10+20)/10 = 3A <= 3A -> ON, branch = 20 W
 *   Load3: (20+20)/10 = 4A >  3A -> OFF (BRANCH_LIMIT_EXCEEDED)
 */
void testIncrementalBranchCurrent() {
    printSection("TEST 10 - INCREMENTAL BRANCH CURRENT");

    const Load::MacAddress mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x61};

    BestFirstSearch search;
    /* wQ tiny and strictly decreasing priority forces a deterministic
       extraction order (Load1, Load2, Load3) without changing the
       electrical quantities under test. */
    search.setSearchScoreWeights(makeWeights(1.0F, 0.0F, 0.0F, 0.001F, 0.0F, 10U));
    search.startSearch(makePlanningState(90.0F, 10.0F, 20.0F, 10.0F, 1000.0F, 1000.0F, 1000.0F, 1000.0F, 0.0F));
    search.registerBranch(BRANCH_0, 0.0F, 3.0F);

    Load load1(Load::Id{mac, 1U}, "Load1", LoadPower{10.0F, 20.0F}, 10U, LoadMode::Auto::ON);
    Load load2(Load::Id{mac, 2U}, "Load2", LoadPower{10.0F, 20.0F}, 9U, LoadMode::Auto::ON);
    Load load3(Load::Id{mac, 3U}, "Load3", LoadPower{10.0F, 20.0F}, 8U, LoadMode::Auto::ON);

    search.addLoad(load1, BRANCH_0, 0.0F);
    search.addLoad(load2, BRANCH_0, 0.0F);
    search.addLoad(load3, BRANCH_0, 0.0F);

    reportCheck("Search runs to completion", search.run());

    reportCheck("Load1 admitted: (0+20)/10 = 2A <= 3A", search.isLoadSelectedToBeOn(0U));
    reportCheck("Load2 admitted: (10+20)/10 = 3A <= 3A", search.isLoadSelectedToBeOn(1U));
    reportCheck("Load3 rejected: (20+20)/10 = 4A > 3A, BRANCH_LIMIT_EXCEEDED",
                !search.isLoadSelectedToBeOn(2U) &&
                search.getLoadSelectionRejectionReason(2U) == BestFirstSearch::BRANCH_LIMIT_EXCEEDED);
    reportCheck("Final branch committed power is 10 + 10 = 20 W",
                nearValue(search.getBranchCommittedPowerWatts(BRANCH_0), 20.0F));
}

/**
 * TEST 11 - SCHEDULE FUTURE PENALTY r_i AND UNCHANGED PRIORITY
 * r_i is supplied to addLoad() exactly as LoadScheduleEvaluator would
 * compute it (r_i = a_i(1 - d_i), Equation 4.32) for each of its three
 * cases, and h(i) reflects it while priority and schedule stay two
 * separate additive terms.
 */
void testScheduleFuturePenaltyAndPriorityIsUnchanged() {
    printSection("TEST 11 - SCHEDULE FUTURE PENALTY r_i");

    const Load::MacAddress mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x71};

    auto runSingleCandidate = [&](const char* caseName, float scheduleFuturePenalty, float expectedHeuristicCost) {
        BestFirstSearch search;
        /* wQ=1, wT=1, Wmax=10, priority=7 -> q_i=0.7, (1-q_i)=0.3. */
        search.setSearchScoreWeights(makeWeights(0.0F, 0.0F, 0.0F, 1.0F, 1.0F, 10U));
        search.startSearch(makePlanningState(90.0F, 10.0F, 20.0F, 10.0F, 1000.0F, 1000.0F, 100.0F, 100.0F, 0.0F));
        search.registerBranch(BRANCH_0, 0.0F, 1000.0F);

        Load load(Load::Id{mac, 1U}, "ScheduledLoad", LoadPower{5.0F, 5.0F}, 7U, LoadMode::Auto::ON);
        search.addLoad(load, BRANCH_0, scheduleFuturePenalty);
        search.run();

        char message[128] = {};
        std::snprintf(message, sizeof(message), "%s: h(i) = 0.3 + wT*r_i = %.2f", caseName, expectedHeuristicCost);
        reportCheck(message, nearValue(search.getLoadHeuristicCost(0U), expectedHeuristicCost));

        char priorityMessage[128] = {};
        std::snprintf(priorityMessage, sizeof(priorityMessage),
                      "%s: Load's stored priority is unchanged by scoring", caseName);
        reportCheck(priorityMessage, load.getPriority() == 7U);
    };

    /* No schedule configured: a_i=0 -> r_i=0. */
    runSingleCandidate("No schedule (r_i=0)", 0.0F, 0.3F);

    /* Schedule configured and already due: a_i=1, d_i=1 -> r_i=a_i(1-d_i)=0. */
    runSingleCandidate("Schedule due (r_i=0)", 0.0F, 0.3F);

    /* Schedule configured but not yet due: a_i=1, d_i=0 -> r_i=1. */
    runSingleCandidate("Schedule not due (r_i=1)", 1.0F, 1.3F);
}

/**
 * TEST 12 - AUTO_ON AND AUTO_OFF ARE BOTH VALID CANDIDATES
 * Two otherwise-identical Loads, one starting AUTO_ON and one starting
 * AUTO_OFF, receive identical scores and identical admission treatment:
 * BestFirstSearch never reads a Load's mode/state.
 */
void testAutoOnAndAutoOffAreEquivalentCandidates() {
    printSection("TEST 12 - AUTO_ON AND AUTO_OFF ARE BOTH VALID CANDIDATES");

    const Load::MacAddress mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x81};

    Load startsOn(Load::Id{mac, 16U}, "StartsOn", LoadPower{10.0F, 10.0F}, 4U, LoadMode::Auto::ON);
    Load startsOff(Load::Id{mac, 17U}, "StartsOff", LoadPower{10.0F, 10.0F}, 4U, LoadMode::Auto::OFF);

    BestFirstSearch search;
    configureEqualWeights(search);
    search.startSearch(makePlanningState(80.0F, 20.0F, 40.0F, 12.0F, 500.0F, 100.0F, 100.0F, 100.0F, 0.0F));
    search.registerBranch(BRANCH_0, 0.0F, 100.0F);
    search.addLoad(startsOn, BRANCH_0, 0.0F);
    search.addLoad(startsOff, BRANCH_0, 0.0F);

    reportCheck("Search runs to completion", search.run());

    reportCheck("Both AUTO_ON and AUTO_OFF Loads with identical power/priority score identically",
                nearValue(search.getLoadFinalScore(0U), search.getLoadFinalScore(1U)));
    reportCheck("Both are admitted (both selected)",
                search.isLoadSelectedToBeOn(0U) && search.isLoadSelectedToBeOn(1U));
    reportCheck("A Load's starting mode/state does not change its own admission outcome",
                search.getLoadSelectionRejectionReason(1U) == BestFirstSearch::NONE);
}

/**
 * TEST 13 - LOADFILTER + AVAILABLEPOWERMANAGER -> BESTFIRSTSEARCH
 * INTEGRATION
 * Only LoadFilter's Auto candidate Loads are handed to BestFirstSearch,
 * on a real branch, using AvailablePowerManager's own
 * getTotalAvailablePowerWatts()/getPowerAvailableForAutoLoadsWatts()/
 * getFixedOnRunningPowerWatts() to build the ElectricalPlanningState —
 * the exact seam described in AvailablePowerManager's and
 * BestFirstSearch's own READMEs.
 */
void testLoadFilterAndAvailablePowerManagerIntegration() {
    printSection("TEST 13 - LOADFILTER + AVAILABLEPOWERMANAGER -> BESTFIRSTSEARCH INTEGRATION");

    const Load::MacAddress centralMac = {0xA4, 0xCF, 0x12, 0x0E, 0x32, 0xC0};
    const Load::MacAddress sittingRoomMac = {0x1C, 0xDB, 0xD4, 0x78, 0xE7, 0xB8};

    Load statusIndicator(Load::Id{centralMac, 25U}, "Central Status Indicator",
                          LoadPower{2.0F, 2.0F}, 1U, LoadMode::Fixed::ON);
    Load coolingFan(Load::Id{centralMac, 26U}, "Central Cooling Fan",
                     LoadPower{6.0F, 9.0F}, 2U, LoadMode::Auto::ON);
    Load router(Load::Id{sittingRoomMac, 17U}, "Router",
                LoadPower{10.0F, 10.0F}, 2U, LoadMode::Fixed::OFF);
    Load fan(Load::Id{sittingRoomMac, 18U}, "Fan",
             LoadPower{18.0F, 25.0F}, 3U, LoadMode::Auto::ON);
    Load waterPump(Load::Id{sittingRoomMac, 19U}, "WaterPump",
                    LoadPower{35.0F, 55.0F}, 4U, LoadMode::Auto::OFF);

    LoadFilter loadFilter;
    loadFilter.addLoad(statusIndicator);
    loadFilter.addLoad(coolingFan);
    loadFilter.addLoad(router);
    loadFilter.addLoad(fan);
    loadFilter.addLoad(waterPump);

    constexpr float testTotalAvailablePowerWatts = 100.0F;

    AvailablePowerManager availablePowerManager;
    const bool availablePowerCalculated =
        availablePowerManager.calculateAvailablePower(testTotalAvailablePowerWatts, loadFilter);

    reportCheck("Fixed ON running power committed before Auto scheduling is 2 W",
                availablePowerCalculated &&
                nearValue(availablePowerManager.getFixedOnRunningPowerWatts(), 2.0F));
    reportCheck("Power remaining for Auto Loads is 100 - 2 = 98 W",
                nearValue(availablePowerManager.getPowerAvailableForAutoLoadsWatts(), 98.0F));

    BestFirstSearch search;
    const bool configured = configureEqualWeights(search);
    const bool started = configured && search.startSearch(makePlanningState(
        80.0F, 20.0F, 40.0F, 12.0F, 1000.0F, 1000.0F,
        availablePowerManager.getTotalAvailablePowerWatts(),
        availablePowerManager.getPowerAvailableForAutoLoadsWatts(),
        availablePowerManager.getFixedOnRunningPowerWatts()));

    reportCheck("BestFirstSearch starts from AvailablePowerManager's own outputs", started);
    reportCheck("BestFirstSearch's constant P_available matches Total Available Power (100 W)",
                nearValue(search.getTotalAvailablePowerWatts(), 100.0F));
    reportCheck("BestFirstSearch's initial P_remaining matches Power Available for Auto Loads (98 W)",
                nearValue(search.getRemainingPowerWatts(), 98.0F));
    reportCheck("BestFirstSearch's initial P_committed matches Fixed ON Running Power (2 W)",
                nearValue(search.getCommittedPowerWatts(), 2.0F));

    search.registerBranch(BRANCH_0, 0.0F, 1000.0F);

    bool everyAutoLoadAdded = started;
    for (std::size_t i = 0U; i < loadFilter.getNumberOfAutoCandidateLoads(); ++i) {
        const Load* autoLoad = loadFilter.getAutoCandidateLoad(i);
        everyAutoLoadAdded = everyAutoLoadAdded && autoLoad != nullptr &&
                             search.addLoad(*autoLoad, BRANCH_0, 0.0F);
    }

    reportCheck("All three Auto candidate Loads (coolingFan, fan, waterPump) were added",
                everyAutoLoadAdded &&
                search.getNumberOfLoadsAdded() == loadFilter.getNumberOfAutoCandidateLoads() &&
                search.getNumberOfLoadsAdded() == 3U);

    for (std::size_t i = 0U; i < loadFilter.getNumberOfAutoCandidateLoads(); ++i) {
        char checkName[96] = {};
        std::snprintf(checkName, sizeof(checkName),
                      "Auto candidate %zu keeps its original Load identity inside the search", i);
        reportCheck(checkName, search.getLoad(i) == loadFilter.getAutoCandidateLoad(i));
    }

    const bool ran = search.run();
    reportCheck("Fixed Loads never entered the search (2 Fixed Loads excluded)",
                ran && search.getNumberOfLoadsAdded() == loadFilter.getNumberOfAutoCandidateLoads());
}

/**
 * TEST 14 - RESET AND BOUNDS SAFETY
 */
void testResetAndBoundsSafety() {
    printSection("TEST 14 - RESET AND BOUNDS SAFETY");

    const Load::MacAddress mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0x91};

    BestFirstSearch search;
    configureEqualWeights(search);
    search.startSearch(makePlanningState(80.0F, 20.0F, 40.0F, 12.0F, 500.0F, 100.0F, 20.0F, 20.0F, 0.0F));
    search.registerBranch(BRANCH_0, 0.0F, 100.0F);

    Load load(Load::Id{mac, 16U}, "Load", LoadPower{5.0F, 5.0F}, 1U, LoadMode::Auto::ON);
    search.addLoad(load, BRANCH_0, 0.0F);
    search.run();

    search.resetSearch();
    reportCheck("resetSearch() clears every added Load", search.getNumberOfLoadsAdded() == 0U);
    reportCheck("resetSearch() clears the power fields",
                nearValue(search.getTotalAvailablePowerWatts(), 0.0F) &&
                nearValue(search.getRemainingPowerWatts(), 0.0F) &&
                nearValue(search.getCommittedPowerWatts(), 0.0F));
    reportCheck("resetSearch() clears branch registrations",
                nearValue(search.getBranchCommittedPowerWatts(BRANCH_0), 0.0F));
    reportCheck("Score weights remain configured after resetSearch(), so startSearch() succeeds again",
                search.startSearch(makePlanningState(80.0F, 20.0F, 40.0F, 12.0F, 500.0F, 100.0F, 30.0F, 30.0F, 0.0F)));
    reportCheck("A BranchId can be registered again after reset (registration was really cleared)",
                search.registerBranch(BRANCH_0, 0.0F, 50.0F));

    reportCheck("getLoad() on an out-of-range index returns nullptr", search.getLoad(99U) == nullptr);
    reportCheck("isLoadSelectedToBeOn() on an out-of-range index returns false", !search.isLoadSelectedToBeOn(99U));
    reportCheck("getLoadSelectionRejectionReason() on an out-of-range index defaults to LOW_BATTERY",
                search.getLoadSelectionRejectionReason(99U) == BestFirstSearch::LOW_BATTERY);
}

/**
 * TEST 15 - DYNAMIC CANDIDATE COUNT
 * BestFirstSearch's containers are all std::vector, so there is no
 * project-imposed maximum candidate count. This adds 120 distinct Auto
 * candidates across two branches with an ample power limit and confirms
 * every one is processed exactly once and admitted.
 */
void testDynamicCandidateCount() {
    printSection("TEST 15 - DYNAMIC CANDIDATE COUNT");

    constexpr std::size_t candidateCount = 120U;
    const Load::MacAddress mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0xA1};

    std::vector<Load> candidates;
    candidates.reserve(candidateCount);
    for (std::size_t i = 0U; i < candidateCount; ++i) {
        char name[24] = {};
        std::snprintf(name, sizeof(name), "Candidate%zu", i);
        candidates.emplace_back(
            Load::Id{mac, static_cast<std::uint8_t>(i)}, name,
            LoadPower{1.0F, 1.0F}, 1U, LoadMode::Auto::ON);
    }

    BestFirstSearch search;
    configureEqualWeights(search);
    /* One watt per candidate is exactly enough for every candidate to fit. */
    search.startSearch(makePlanningState(
        90.0F, 10.0F, 20.0F, 12.0F, 10000.0F, 10000.0F,
        static_cast<float>(candidateCount), static_cast<float>(candidateCount), 0.0F));
    search.registerBranch(BRANCH_0, 0.0F, 10000.0F);
    search.registerBranch(BRANCH_1, 0.0F, 10000.0F);

    bool everyLoadAdded = true;
    for (std::size_t i = 0U; i < candidates.size(); ++i) {
        const BranchId branch = (i % 2U == 0U) ? BRANCH_0 : BRANCH_1;
        everyLoadAdded = everyLoadAdded && search.addLoad(candidates[i], branch, 0.0F);
    }

    reportCheck("All 120 dynamically-created candidates were accepted by addLoad()",
                everyLoadAdded && search.getNumberOfLoadsAdded() == candidateCount);

    reportCheck("Search over 120 candidates runs to completion", search.run());

    std::size_t selectedCount = 0U;
    for (std::size_t i = 0U; i < search.getNumberOfLoadsAdded(); ++i) {
        if (search.isLoadSelectedToBeOn(i)) {
            ++selectedCount;
        }
    }

    reportCheck("Every one of the 120 candidates fits the limit and is selected", selectedCount == candidateCount);
    reportCheck("Remaining power after selecting all 120 one-watt candidates is 0 W",
                nearValue(search.getRemainingPowerWatts(), 0.0F));
    reportCheck("Branch 0 and branch 1 committed power split 60 W / 60 W",
                nearValue(search.getBranchCommittedPowerWatts(BRANCH_0), 60.0F) &&
                nearValue(search.getBranchCommittedPowerWatts(BRANCH_1), 60.0F));
}

/**
 * TEST 16 - getAdmittedLoad() RECONSTRUCTS TRUE BEST-FIRST ADMISSION ORDER
 * addLoad() is called in an order deliberately opposite to what f(i) will
 * rank: the candidate with the WORST (highest) score is added first, the
 * best (lowest) score is added last. getLoad(i)/isLoadSelectedToBeOn(i)
 * therefore report in addLoad() order (index 0 = LoadWorst), but
 * getAdmittedLoad() must report in the true Best-First extraction order
 * (LoadBest first) regardless — this is exactly the ordering a caller
 * dispatching relay ON commands must use (see RelayCommandDispatcher).
 */
void testAdmittedLoadOrderMatchesBestFirstExtraction() {
    printSection("TEST 16 - getAdmittedLoad() RECONSTRUCTS TRUE BEST-FIRST ORDER");

    const Load::MacAddress mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0xB1};

    BestFirstSearch search;
    /* Priority alone drives ordering here (all other weights zero); higher priority -> lower h(i) -> extracted first. */
    search.setSearchScoreWeights(makeWeights(0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 10U));
    search.startSearch(makePlanningState(90.0F, 10.0F, 20.0F, 12.0F, 1000.0F, 1000.0F, 1000.0F, 1000.0F, 0.0F));
    search.registerBranch(BRANCH_0, 0.0F, 1000.0F);

    /* Added worst-first (lowest priority = worst score = added first), deliberately reversed from admission order. */
    Load loadWorst(Load::Id{mac, 1U}, "LoadWorst", LoadPower{1.0F, 1.0F}, 1U, LoadMode::Auto::ON);
    Load loadMiddle(Load::Id{mac, 2U}, "LoadMiddle", LoadPower{1.0F, 1.0F}, 5U, LoadMode::Auto::ON);
    Load loadBest(Load::Id{mac, 3U}, "LoadBest", LoadPower{1.0F, 1.0F}, 10U, LoadMode::Auto::ON);

    search.addLoad(loadWorst, BRANCH_0, 0.0F);   // addLoad() index 0
    search.addLoad(loadMiddle, BRANCH_0, 0.0F);  // addLoad() index 1
    search.addLoad(loadBest, BRANCH_0, 0.0F);    // addLoad() index 2

    reportCheck("Search runs to completion", search.run());

    reportCheck("getLoad(0) still reflects addLoad() order (LoadWorst), not admission order",
                search.getLoad(0U) == &loadWorst);

    reportCheck("All three candidates were admitted", search.getNumberOfAdmittedLoads() == 3U);

    reportCheck("getAdmittedLoad(0) is LoadBest (extracted first: highest priority -> smallest f(i))",
                search.getAdmittedLoad(0U) == &loadBest);
    reportCheck("getAdmittedLoad(1) is LoadMiddle (extracted second)",
                search.getAdmittedLoad(1U) == &loadMiddle);
    reportCheck("getAdmittedLoad(2) is LoadWorst (extracted last: lowest priority -> largest f(i))",
                search.getAdmittedLoad(2U) == &loadWorst);

    reportCheck("getAdmittedLoad() out of range returns nullptr", search.getAdmittedLoad(3U) == nullptr);

    search.resetSearch();
    reportCheck("resetSearch() clears the admission-order record too", search.getNumberOfAdmittedLoads() == 0U);
}

/**
 * TEST 17 - PUBLIC STATIC checkFeasibility() FOR PRE-ACTUATION SAFETY RE-CHECKS
 * Exercises BestFirstSearch::checkFeasibility(FeasibilityInputs) directly,
 * without running a search at all — this is exactly what a caller
 * re-verifying feasibility immediately before sending an ON command needs
 * (Section "Add The Documented Safety Re-Check Immediately Before Each ON
 * Command"): the same Algorithm 4.4 guard, callable standalone against a
 * freshly re-read electrical snapshot.
 */
void testStaticFeasibilityCheckForPreActuationRecheck() {
    printSection("TEST 17 - STATIC checkFeasibility() FOR PRE-ACTUATION SAFETY RE-CHECKS");

    auto makeFeasible = []() {
        BestFirstSearch::FeasibilityInputs inputs{};
        inputs.stateOfChargePercent = 80.0F;
        inputs.minimumStateOfChargePercent = 20.0F;
        inputs.candidateRunningPowerWatts = 5.0F;
        inputs.candidatePeakPowerWatts = 8.0F;
        inputs.remainingPowerWatts = 50.0F;
        inputs.committedPowerWatts = 10.0F;
        inputs.maximumBatteryPowerWatts = 200.0F;
        inputs.batteryBusVoltageVolts = 12.0F;
        inputs.maximumMainCurrentAmps = 50.0F;
        inputs.branchCommittedPowerWatts = 0.0F;
        inputs.branchMaximumCurrentAmps = 10.0F;
        return inputs;
    };

    reportCheck("A fully feasible snapshot returns NONE",
                BestFirstSearch::checkFeasibility(makeFeasible()) == BestFirstSearch::NONE);

    {
        BestFirstSearch::FeasibilityInputs inputs = makeFeasible();
        inputs.stateOfChargePercent = 20.0F; // == SoC_min
        reportCheck("SoC <= SoC_min standalone -> LOW_BATTERY",
                    BestFirstSearch::checkFeasibility(inputs) == BestFirstSearch::LOW_BATTERY);
    }
    {
        BestFirstSearch::FeasibilityInputs inputs = makeFeasible();
        inputs.candidateRunningPowerWatts = 60.0F; // > remainingPowerWatts (50)
        reportCheck("P_i > P_remaining standalone -> POWER_LIMIT_EXCEEDED",
                    BestFirstSearch::checkFeasibility(inputs) == BestFirstSearch::POWER_LIMIT_EXCEEDED);
    }
    {
        BestFirstSearch::FeasibilityInputs inputs = makeFeasible();
        inputs.committedPowerWatts = 195.0F; // + 8 peak > 200 maximumBatteryPowerWatts
        reportCheck("P_committed + P_i_peak > P_battery,max standalone -> BATTERY_CURRENT_LIMIT",
                    BestFirstSearch::checkFeasibility(inputs) == BestFirstSearch::BATTERY_CURRENT_LIMIT);
    }
    {
        BestFirstSearch::FeasibilityInputs inputs = makeFeasible();
        inputs.maximumMainCurrentAmps = 1.0F; // (10+8)/12 = 1.5A > 1A
        reportCheck("I_main,start > I_main,max standalone -> MAIN_LIMIT_EXCEEDED",
                    BestFirstSearch::checkFeasibility(inputs) == BestFirstSearch::MAIN_LIMIT_EXCEEDED);
    }
    {
        BestFirstSearch::FeasibilityInputs inputs = makeFeasible();
        inputs.branchMaximumCurrentAmps = 0.5F; // (0+8)/12 = 0.667A > 0.5A
        reportCheck("I_branch,start > I_branch,max standalone -> BRANCH_LIMIT_EXCEEDED",
                    BestFirstSearch::checkFeasibility(inputs) == BestFirstSearch::BRANCH_LIMIT_EXCEEDED);
    }

    reportCheck("checkFeasibility() is callable with no search ever started (fully static)", true);
}

/**
 * TEST 18 - CONFIGURED MODE IS NEVER MUTATED BY A BEST-FIRST DECISION
 * Reproduces the exact orchestration pattern src/central/main.cpp's
 * Optimisation Task must use after search.run(): write the admission
 * result onto Load::setTargetRelayState(), never onto Load::setMode().
 * An AUTO_OFF Load that gets selected must end this cycle with target=ON
 * but configured mode still exactly AUTO_OFF; an AUTO_ON Load that gets
 * rejected must end with target=OFF but configured mode still AUTO_ON.
 */
void testConfiguredModeNeverMutatedByBestFirstDecision() {
    printSection("TEST 18 - CONFIGURED MODE IS NEVER MUTATED BY A BEST-FIRST DECISION");

    const Load::MacAddress mac = {0x02, 0x00, 0x00, 0x00, 0x00, 0xC1};

    BestFirstSearch search;
    configureEqualWeights(search);
    /* Generous limit: WaterPump (35W) fits, but nothing else competes for it, so it is admitted. */
    search.startSearch(makePlanningState(80.0F, 20.0F, 40.0F, 12.0F, 1000.0F, 1000.0F, 100.0F, 100.0F, 0.0F));
    search.registerBranch(BRANCH_0, 0.0F, 1000.0F);

    /* Configured AUTO_OFF, expected to be SELECTED this cycle. */
    Load waterPump(Load::Id{mac, 19U}, "WaterPump", LoadPower{35.0F, 55.0F}, 4U, LoadMode::Auto::OFF);
    search.addLoad(waterPump, BRANCH_0, 0.0F);

    reportCheck("Search runs to completion", search.run());
    reportCheck("WaterPump (35W, generous 100W limit) is admitted this cycle", search.isLoadSelectedToBeOn(0U));

    /* Exactly the orchestration pattern central/main.cpp must use: */
    waterPump.setLastBestFirstRejectionReason(search.getLoadSelectionRejectionReason(0U));
    waterPump.setTargetRelayState(search.isLoadSelectedToBeOn(0U));

    reportCheck("Configured mode remains exactly AUTO_OFF after being selected",
                waterPump.getMode() == LoadMode::Auto::OFF);
    reportCheck("isOn()/isOff() (which read configured mode) still report OFF",
                waterPump.isOff() && !waterPump.isOn());
    reportCheck("Target relay state is ON, reflecting this cycle's Best-First decision",
                waterPump.getTargetRelayState());

    /* Now the rejected direction: configured AUTO_ON, but limit forces rejection. */
    BestFirstSearch tightSearch;
    configureEqualWeights(tightSearch);
    tightSearch.startSearch(makePlanningState(80.0F, 20.0F, 40.0F, 12.0F, 1000.0F, 1000.0F, 5.0F, 5.0F, 0.0F));
    tightSearch.registerBranch(BRANCH_0, 0.0F, 1000.0F);

    Load fan(Load::Id{mac, 17U}, "Fan", LoadPower{18.0F, 25.0F}, 6U, LoadMode::Auto::ON);
    tightSearch.addLoad(fan, BRANCH_0, 0.0F);

    reportCheck("Search runs to completion", tightSearch.run());
    reportCheck("Fan (18W, only 5W limit) is rejected this cycle", !tightSearch.isLoadSelectedToBeOn(0U));

    fan.setLastBestFirstRejectionReason(tightSearch.getLoadSelectionRejectionReason(0U));
    fan.setTargetRelayState(tightSearch.isLoadSelectedToBeOn(0U));

    reportCheck("Configured mode remains exactly AUTO_ON after being rejected", fan.getMode() == LoadMode::Auto::ON);
    reportCheck("isOn()/isOff() (which read configured mode) still report ON", fan.isOn() && !fan.isOff());
    reportCheck("Target relay state is OFF, reflecting this cycle's Best-First rejection", !fan.getTargetRelayState());
    reportCheck("Rejection reason (POWER_LIMIT_EXCEEDED) was recorded",
                fan.getLastBestFirstRejectionReason() == BestFirstSearch::POWER_LIMIT_EXCEEDED);
}

/*
 * TEST 19 - REALISTIC MULTI-NODE INSTALLATION ACROSS VARYING BATTERY
 * CAPACITY AND STATE OF CHARGE
 *
 * Everything above hand-derives one scoring/rejection case at a time
 * directly from the Chapter 4 equations. This test instead builds one
 * fixed, realistic 20-Load installation - 5 Loads owned by Central, 5 by
 * one Smart Node, 10 by a second Smart Node, a mix of Fixed/Auto modes,
 * wattages from 2W to 1500W, and two Loads with a non-zero future-schedule
 * penalty - and runs the *entire* pipeline a real optimisation cycle uses
 * (SafePowerLimitCalculator -> AvailablePowerManager -> BestFirstSearch) across
 * every combination of battery capacity (20/100/1000 Ah) and State of
 * Charge (25/50/75/100%), 12 scenarios in total.
 *
 * For every scenario it prints a complete, readable report - starting
 * battery capacity/energy, Fixed-only remaining limit, then every single
 * Load's configured (initial) state next to Best-First's this-cycle
 * (new) state and rejection reason, then the final committed/remaining
 * power - and asserts the invariants that must hold no matter how the
 * battery scenario changes: every Fixed Load's configured mode is
 * completely unaffected by the battery scenario, no admitted Load ever
 * exceeds the limit it was admitted under, and a strictly larger
 * capacity/SoC combination never admits *less* total power than a
 * strictly smaller one.
 */
namespace {

struct LoadSpec {
    const char* name;
    Load::MacAddress owner;
    std::uint8_t relayPin;
    float runningWatts;
    float startupWatts;
    std::uint16_t priority;
    LoadMode::Value mode;
    /** r_i supplied directly to addLoad(), standing in for what LoadScheduleEvaluator would have computed (see file header). */
    float schedulePenalty;
};

const char* loadModeText(LoadMode::Value mode) {
    if (mode == LoadMode::Fixed::ON) return "FIXED_ON";
    if (mode == LoadMode::Fixed::OFF) return "FIXED_OFF";
    if (mode == LoadMode::Auto::ON) return "AUTO_ON";
    return "AUTO_OFF";
}

const char* rejectionReasonText(std::uint8_t reason) {
    switch (reason) {
        case BestFirstSearch::NONE: return "-";
        case BestFirstSearch::LOW_BATTERY: return "LOW_BATTERY";
        case BestFirstSearch::POWER_LIMIT_EXCEEDED: return "POWER_LIMIT_EXCEEDED";
        case BestFirstSearch::BATTERY_CURRENT_LIMIT: return "BATTERY_CURRENT_LIMIT";
        case BestFirstSearch::MAIN_LIMIT_EXCEEDED: return "MAIN_LIMIT_EXCEEDED";
        case BestFirstSearch::BRANCH_LIMIT_EXCEEDED: return "BRANCH_LIMIT_EXCEEDED";
        default: return "UNKNOWN";
    }
}

/**
 * What this exact installation would draw with no power-limit
 * coordination at all - every Load simply left running at its own
 * currently-configured state (Fixed-ON Loads, plus every Auto Load
 * currently AUTO_ON), the way a plain relay board with manual switches
 * and no Central intelligence would behave. AUTO_OFF Loads are excluded
 * here too, same as they would be with no coordinator: nothing turns a
 * Load on that its own switch position says is off.
 */
float naiveConfiguredOnDemandWatts(const std::vector<LoadSpec>& specs) {
    float total = 0.0F;
    for (const LoadSpec& spec : specs) {
        if (spec.mode == LoadMode::Fixed::ON || spec.mode == LoadMode::Auto::ON) {
            total += spec.runningWatts;
        }
    }
    return total;
}

/*
 * 4 owners (Central + 3 Smart Nodes), 5 Loads each, at least 4 of every
 * owner's 5 Loads are Auto candidates (the only kind Best-First actually
 * scores) - only one Fixed Load per owner, so no owner's Loads are ever
 * entirely decided before Best-First even runs.
 */
std::vector<LoadSpec> buildRealisticInstallation() {
    const Load::MacAddress central = {0xA4, 0xCF, 0x12, 0x0E, 0x32, 0xC0};
    const Load::MacAddress nodeA   = {0x1C, 0xDB, 0xD4, 0x78, 0xE7, 0xB8}; // "Living Room" Smart Node
    const Load::MacAddress nodeB   = {0x1C, 0xDB, 0xD4, 0x78, 0xE7, 0xC2}; // "Garden" Smart Node
    const Load::MacAddress nodeC   = {0x1C, 0xDB, 0xD4, 0x78, 0xE7, 0xD3}; // "Workshop" Smart Node

    return {
        // -- Central: 5 Loads (4 Auto + 1 Fixed) --
        {"Central Status Light",   central, 10U,   2.0F,   2.0F,  1U, LoadMode::Fixed::ON, 0.0F},
        {"Central Cooling Fan",    central, 11U,  15.0F,  25.0F,  5U, LoadMode::Auto::ON,  0.0F},
        {"Central Aux Charger",    central, 12U,  25.0F,  25.0F,  7U, LoadMode::Auto::ON,  0.0F},
        {"Central Backup Pump",    central, 13U,  40.0F,  60.0F,  6U, LoadMode::Auto::OFF, 0.5F}, // schedule not yet due
        {"Central Monitor Display",central, 14U,  10.0F,  10.0F,  4U, LoadMode::Auto::ON,  0.0F},

        // -- Node A ("Living Room"): 5 Loads (4 Auto + 1 Fixed) --
        {"Kitchen Fridge",        nodeA,   16U,  60.0F, 150.0F,  1U, LoadMode::Fixed::ON, 0.0F},
        {"Living Room Lights",    nodeA,   17U,  18.0F,  18.0F,  4U, LoadMode::Auto::ON,  0.0F},
        {"Living Room TV",        nodeA,   18U,  90.0F, 100.0F,  6U, LoadMode::Auto::ON,  0.0F},
        {"Kitchen Kettle",        nodeA,   19U, 800.0F, 900.0F,  9U, LoadMode::Auto::OFF, 0.0F},
        {"Living Room Fan",       nodeA,   20U,  30.0F,  45.0F,  3U, LoadMode::Auto::ON,  0.0F},

        // -- Node B ("Garden"): 5 Loads (4 Auto + 1 Fixed) --
        {"Garage Door Opener",    nodeB,   22U,   5.0F,   5.0F,  2U, LoadMode::Fixed::OFF, 0.0F},
        {"Bedroom Lights",        nodeB,   23U,  12.0F,  12.0F,  4U, LoadMode::Auto::ON,   0.0F},
        {"Bedroom AC",            nodeB,   24U, 350.0F, 450.0F,  8U, LoadMode::Auto::ON,   0.0F},
        {"Garage Lights",         nodeB,   25U,  20.0F,  20.0F,  5U, LoadMode::Auto::ON,   0.0F},
        {"Water Heater",          nodeB,   26U, 500.0F, 550.0F, 10U, LoadMode::Auto::OFF,  0.8F}, // schedule not yet due

        // -- Node C ("Workshop"): 5 Loads (4 Auto + 1 Fixed) --
        {"Sump Pump",             nodeC,   28U,  90.0F, 200.0F,  1U, LoadMode::Fixed::ON, 0.0F},
        {"Washing Machine",       nodeC,   29U, 400.0F, 600.0F,  7U, LoadMode::Auto::OFF, 0.0F},
        {"Pool Pump",             nodeC,   30U, 250.0F, 300.0F,  6U, LoadMode::Auto::ON,  0.0F},
        {"Outdoor Lights",        nodeC,   31U,  15.0F,  15.0F,  3U, LoadMode::Auto::ON,  0.0F},
        {"EV Charger",            nodeC,   32U,1500.0F,1500.0F, 10U, LoadMode::Auto::OFF, 0.0F},
    };
}

void runInstallationScenario(const std::vector<LoadSpec>& specs, float batteryCapacityAmpHours,
                              float stateOfChargePercent, float& totalCommittedPowerOut) {
    constexpr float MINIMUM_SOC_PERCENT = 20.0F;
    constexpr float WARNING_SOC_PERCENT = 40.0F;
    constexpr float NOMINAL_VOLTAGE_VOLTS = 12.0F;
    constexpr float TARGET_RUNTIME_HOURS = 4.0F;
    constexpr float MAX_BATTERY_DISCHARGE_CURRENT_AMPS = 40.0F;
    constexpr float MAX_MAIN_CURRENT_AMPS = 30.0F;
    constexpr float SAFETY_FACTOR = 0.9F;
    constexpr float GENEROUS_BRANCH_MAX_CURRENT_AMPS = 20.0F;

    std::printf("\n----------------------------------------------------------------------\n");
    std::printf("SCENARIO: batteryCapacity=%.0f Ah  SoC=%.0f%%\n", batteryCapacityAmpHours, stateOfChargePercent);
    std::printf("----------------------------------------------------------------------\n");

    SafePowerLimitCalculator::Inputs safePowerLimitInputs{};
    safePowerLimitInputs.stateOfChargePercent = stateOfChargePercent;
    safePowerLimitInputs.minimumStateOfChargePercent = MINIMUM_SOC_PERCENT;
    safePowerLimitInputs.nominalBatteryVoltageVolts = NOMINAL_VOLTAGE_VOLTS;
    safePowerLimitInputs.batteryCapacityAmpHours = batteryCapacityAmpHours;
    safePowerLimitInputs.targetRuntimeHours = TARGET_RUNTIME_HOURS;
    safePowerLimitInputs.batteryBusVoltageVolts = NOMINAL_VOLTAGE_VOLTS;
    safePowerLimitInputs.maximumBatteryDischargeCurrentAmps = MAX_BATTERY_DISCHARGE_CURRENT_AMPS;
    safePowerLimitInputs.maximumMainCurrentAmps = MAX_MAIN_CURRENT_AMPS;
    safePowerLimitInputs.safetyFactor = SAFETY_FACTOR;

    SafePowerLimitCalculator limit;
    const bool safePowerLimitCalculated = limit.calculate(safePowerLimitInputs);
    reportCheck("SafePowerLimitCalculator accepts this scenario's inputs", safePowerLimitCalculated);
    if (!safePowerLimitCalculated) {
        totalCommittedPowerOut = 0.0F;
        return;
    }

    const float ratedEnergyWh = limit.getRatedEnergyWattHours();
    const float usableEnergyWh = limit.getUsableEnergyWattHours();
    const float totalAvailablePowerWatts = limit.getAvailablePowerWatts();
    std::printf("  Battery: E_rated=%.1f Wh  E_usable=%.1f Wh  P_available=%.1f W\n",
                ratedEnergyWh, usableEnergyWh, totalAvailablePowerWatts);

    /* Build fresh Load objects for this scenario - BestFirstSearch keeps its own references alive only for this run(). */
    std::vector<Load> loads;
    loads.reserve(specs.size());
    for (const LoadSpec& spec : specs) {
        loads.emplace_back(Load::Id{spec.owner, spec.relayPin}, spec.name,
                            LoadPower{spec.runningWatts, spec.startupWatts}, spec.priority, spec.mode);
    }

    LoadFilter loadFilter;
    for (const Load& load : loads) {
        reportCheck("LoadFilter accepts every Load in the installation", loadFilter.addLoad(load));
    }

    AvailablePowerManager availablePowerManager;
    reportCheck("AvailablePowerManager accepts this scenario's P_available",
                availablePowerManager.calculateAvailablePower(totalAvailablePowerWatts, loadFilter));

    const float fixedOnPowerWatts = availablePowerManager.getFixedOnRunningPowerWatts();
    const float powerForAutoLoadsWatts = availablePowerManager.getPowerAvailableForAutoLoadsWatts();
    std::printf("  Fixed ON Loads commit %.1f W, leaving %.1f W for Auto Loads before Best-First runs\n",
                fixedOnPowerWatts, powerForAutoLoadsWatts);

    BestFirstSearch search;
    reportCheck("Search score weights configured", configureEqualWeights(search));
    const bool searchStarted = search.startSearch(makePlanningState(
        stateOfChargePercent, MINIMUM_SOC_PERCENT, WARNING_SOC_PERCENT, NOMINAL_VOLTAGE_VOLTS,
        limit.getMaximumBatteryPowerWatts(), MAX_MAIN_CURRENT_AMPS,
        totalAvailablePowerWatts, powerForAutoLoadsWatts, fixedOnPowerWatts));
    reportCheck("Search starts for every SoC/capacity combination in range", searchStarted);
    if (!searchStarted) {
        totalCommittedPowerOut = 0.0F;
        return;
    }

    /*
     * Auto candidates come from loadFilter.getAutoCandidateLoad() - the
     * same accessor src/central/main.cpp's runOptimizationCycle() actually
     * calls - never re-derived here by asking each Load isAuto() directly.
     * That is the one piece of this pipeline actually under test alongside
     * BestFirstSearch itself: if LoadFilter's own classification ever
     * disagreed with a Load's raw mode, going through the real accessor is
     * what would catch it.
     *
     * autoLoadSpecIndices[searchIndex] == i is the only source of truth
     * mapping a search position back to its LoadSpec - a failed addLoad()
     * (e.g. an out-of-range schedule penalty) must not silently shift
     * every later index, so it is only recorded on success.
     */
    std::vector<std::size_t> autoLoadSpecIndices;
    for (std::size_t candidateIndex = 0U; candidateIndex < loadFilter.getNumberOfAutoCandidateLoads(); ++candidateIndex) {
        const Load* candidate = loadFilter.getAutoCandidateLoad(candidateIndex);
        reportCheck("Every Auto candidate LoadFilter returns is non-null", candidate != nullptr);
        if (candidate == nullptr) continue;

        std::size_t specIndex = specs.size();
        for (std::size_t i = 0U; i < specs.size(); ++i) {
            if (specs[i].owner == candidate->getMacAddress() && specs[i].relayPin == candidate->getRelayPin()) {
                specIndex = i;
                break;
            }
        }
        reportCheck("Every Auto candidate LoadFilter returns matches one of the installation's own Loads",
                    specIndex < specs.size());
        if (specIndex >= specs.size()) continue;

        const BestFirstSearch::BranchId branchId{specs[specIndex].owner, specs[specIndex].relayPin};
        reportCheck("Branch registered for every Auto candidate",
                    search.registerBranch(branchId, 0.0F, GENEROUS_BRANCH_MAX_CURRENT_AMPS));
        const bool added = search.addLoad(*candidate, branchId, specs[specIndex].schedulePenalty);
        reportCheck("Auto candidate added to the search", added);
        if (added) {
            autoLoadSpecIndices.push_back(specIndex);
        }
    }

    reportCheck("Search runs to completion", search.run());

    std::printf("  %-24s %-9s %-10s %-9s %-24s\n", "Load", "Owner", "Initial", "New", "Reason (if rejected)");
    float totalCommittedThisScenario = fixedOnPowerWatts;
    for (std::size_t i = 0U; i < loads.size(); ++i) {
        const char* owner = specs[i].owner == specs[0].owner ? "Central"
                            : (specs[i].owner == specs[5].owner ? "NodeA"
                               : (specs[i].owner == specs[10].owner ? "NodeB" : "NodeC"));
        const char* initialState = loadModeText(specs[i].mode);

        if (!loads[i].isAuto()) {
            std::printf("  %-24s %-9s %-10s %-9s %-24s\n", specs[i].name, owner, initialState, initialState, "-");
            continue;
        }

        std::size_t searchIndex = 0U;
        bool foundInSearch = false;
        for (std::size_t k = 0U; k < autoLoadSpecIndices.size(); ++k) {
            if (autoLoadSpecIndices[k] == i) {
                searchIndex = k;
                foundInSearch = true;
                break;
            }
        }

        if (!foundInSearch) {
            /* addLoad() rejected this candidate outright (see the checks above) - never evaluated, not the same as OFF. */
            std::printf("  %-24s %-9s %-10s %-9s %-24s\n", specs[i].name, owner, initialState,
                        "N/A", "addLoad() rejected");
            reportCheck("Configured mode is never mutated by the Best-First decision",
                        loads[i].getMode() == specs[i].mode);
            continue;
        }

        const bool selected = search.isLoadSelectedToBeOn(searchIndex);
        const std::uint8_t reason = search.getLoadSelectionRejectionReason(searchIndex);
        std::printf("  %-24s %-9s %-10s %-9s %-24s\n", specs[i].name, owner, initialState,
                    selected ? "ON" : "OFF", rejectionReasonText(reason));
        if (selected) {
            totalCommittedThisScenario += specs[i].runningWatts;
        }

        reportCheck("Configured mode is never mutated by the Best-First decision",
                    loads[i].getMode() == specs[i].mode);
    }

    std::printf("  P_remaining=%.1f W  P_committed=%.1f W (Fixed %.1f W + admitted Auto)\n",
                search.getRemainingPowerWatts(), search.getCommittedPowerWatts(), fixedOnPowerWatts);

    /*
     * BestFirstSearch does not own Fixed Load safety - a Fixed-ON Load
     * commits its power regardless of limit (Section "Fixed Loads are
     * never Best-First Search candidates"; shedding an over-limit Fixed
     * Load is src/central/main.cpp's separate critical-protection logic,
     * outside this library). So P_committed can legitimately exceed
     * P_available when Fixed Loads alone already do, at low battery
     * capacity/SoC (see this installation's 152W of Fixed-ON Loads). What
     * BestFirstSearch does own is the Auto-load allocation: it must never
     * admit more Auto power than powerForAutoLoadsWatts, the limit left
     * over after Fixed Loads.
     */
    reportCheck("Admitted Auto power never exceeds the limit left after Fixed Loads",
                search.getCommittedPowerWatts() - fixedOnPowerWatts <= powerForAutoLoadsWatts + 0.001F);
    reportCheck("Best-First's own P_committed matches this report's independently-summed total",
                nearValue(search.getCommittedPowerWatts(), totalCommittedThisScenario, 0.01F));

    /*
     * Battery runtime comparison: how long E_usable actually lasts running
     * only what Best-First admitted, versus what it would have lasted
     * running this installation's naive "every switch left in its current
     * position" demand instead - both computed the same way (Wh / W), no
     * assumed duty cycle, no invented numbers. TARGET_RUNTIME_HOURS
     * (T_target) is the same constant SafePowerLimitCalculator itself used to
     * derive P_runtime (Equation 4.11) for this scenario's P_available, so
     * comparing against it is comparing against the system's own design
     * goal, not an arbitrary window.
     */
    const float naiveDemandWatts = naiveConfiguredOnDemandWatts(specs);
    const float naiveRuntimeHours = naiveDemandWatts > 0.0F ? usableEnergyWh / naiveDemandWatts : 0.0F;
    const float admittedRuntimeHours =
        totalCommittedThisScenario > 0.0F ? usableEnergyWh / totalCommittedThisScenario : 0.0F;
    std::printf("  Battery runtime: naive (no coordination, %.1f W) lasts %.2f h; "
                "Best-First (%.1f W) lasts %.2f h (T_target=%.1f h)\n",
                naiveDemandWatts, naiveRuntimeHours, totalCommittedThisScenario, admittedRuntimeHours,
                TARGET_RUNTIME_HOURS);
    reportCheck("Runtime figures are computed, not fabricated: naive runtime x naive demand reproduces E_usable",
                naiveDemandWatts <= 0.0F || nearValue(naiveRuntimeHours * naiveDemandWatts, usableEnergyWh, 0.05F));

    totalCommittedPowerOut = totalCommittedThisScenario;
}

void testRealisticMultiNodeInstallationAcrossBatteryScenarios() {
    printSection("TEST 19 - REALISTIC 20-LOAD, 3-NODE INSTALLATION ACROSS BATTERY CAPACITY x SOC");

    const std::vector<LoadSpec> installation = buildRealisticInstallation();
    reportCheck("Installation has exactly 20 Loads (5 Central + 5 NodeA + 10 NodeB)",
                installation.size() == 20U);

    const float capacities[] = {20.0F, 100.0F, 1000.0F};
    const float statesOfCharge[] = {25.0F, 50.0F, 75.0F, 100.0F};

    /*
     * previousCommitted tracks the last scenario's total committed power in
     * (capacity, SoC) iteration order - both loops go from smallest to
     * largest, so a strictly larger capacity or SoC must never admit
     * strictly less total power than a strictly smaller one on the same
     * fixed installation, since the limit can only grow.
     */
    float previousCommittedForCapacity = -1.0F;
    for (float capacity : capacities) {
        float previousCommittedForSoc = -1.0F;
        for (float soc : statesOfCharge) {
            float committedThisScenario = 0.0F;
            runInstallationScenario(installation, capacity, soc, committedThisScenario);

            if (previousCommittedForSoc >= 0.0F) {
                reportCheck("Higher SoC at the same capacity never admits less total power",
                            committedThisScenario >= previousCommittedForSoc - 0.01F);
            }
            previousCommittedForSoc = committedThisScenario;
        }

        if (previousCommittedForCapacity >= 0.0F) {
            reportCheck("Higher capacity at the same (highest) SoC never admits less total power",
                        previousCommittedForSoc >= previousCommittedForCapacity - 0.01F);
        }
        previousCommittedForCapacity = previousCommittedForSoc;
    }
}

} // namespace (realistic multi-node scenario)

} // namespace

int main() {
    std::printf("KILOWATTS LOAD + LOADFILTER + AVAILABLEPOWERMANAGER + BESTFIRSTSEARCH HOST TEST REPORT\n");
    std::printf("Targets the Chapter 4 lib/BestFirstSearch implementation (Sections 4.6.2-4.6.4).\n");

    testLoadDataModel();
    testLoadFilterClassification();
    testSearchScoreWeightValidation();
    testElectricalPlanningStateValidation();
    testRegisterBranchValidation();
    testBranchIdentityIsMacPlusRelayPin();
    testAddLoadValidation();
    testChapter4WorkedExample();
    testConstraintGuardRejectionReasons();
    testSequentialAllocation();
    testIncrementalBranchCurrent();
    testScheduleFuturePenaltyAndPriorityIsUnchanged();
    testAutoOnAndAutoOffAreEquivalentCandidates();
    testLoadFilterAndAvailablePowerManagerIntegration();
    testResetAndBoundsSafety();
    testDynamicCandidateCount();
    testAdmittedLoadOrderMatchesBestFirstExtraction();
    testStaticFeasibilityCheckForPreActuationRecheck();
    testConfiguredModeNeverMutatedByBestFirstDecision();
    testRealisticMultiNodeInstallationAcrossBatteryScenarios();

    printSection("FINAL TEST SUMMARY");
    std::printf("Passed checks: %zu\n", passedChecks);
    std::printf("Failed checks: %zu\n", failedChecks);
    std::printf("OVERALL RESULT: %s\n", failedChecks == 0U ? "PASS" : "FAIL");

    return failedChecks == 0U ? 0 : 1;
}
