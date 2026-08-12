/**
 * @file main.cpp
 * @brief Runs ESP32 unit and integration tests for Load and BestFirstSearch.
 *
 * The tests follow the document-defined separation of responsibilities:
 * Load stores and validates one managed DC load, while BestFirstSearch receives
 * only validated, healthy AUTO loads and performs Candidate Evaluation,
 * Constraint Guard checks, and Best-First scheduling.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 11 August 2026
 */

#include "Load.h"
#include "BestFirstSearch.h"

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <inttypes.h>
#include <limits>

using kilowatts::BestFirstSearch;
using kilowatts::Load;
using kilowatts::LoadAvailabilityReason;
using kilowatts::LoadMode;
using kilowatts::LoadState;

static std::size_t passedChecks = 0U;
static std::size_t failedChecks = 0U;

static bool nearValue(float actual, float expected, float tolerance = 0.0001F) {
    return std::fabs(actual - expected) <= tolerance;
}

static const char* recordResult(bool passed) {
    if (passed) {
        ++passedChecks;
        return "PASS";
    }

    ++failedChecks;
    return "FAIL";
}

static bool reportCheck(const char* name, bool passed) {
    std::printf("%-66s %s\n", name, recordResult(passed));
    return passed;
}

static void printSection(const char* title) {
    std::printf("\n======================================================================\n");
    std::printf("%s\n", title);
    std::printf("======================================================================\n");
}

static const char* loadModeText(LoadMode mode) {
    return mode == LoadMode::FIXED ? "FIXED" : "AUTO";
}

static const char* loadStateText(LoadState state) {
    return state == LoadState::ON ? "ON" : "OFF";
}

static const char* availabilityReasonText(LoadAvailabilityReason reason) {
    switch (reason) {
        case LoadAvailabilityReason::NONE:
            return "NONE";
        case LoadAvailabilityReason::HARDWARE_FAULT:
            return "HARDWARE_FAULT";
        case LoadAvailabilityReason::INVALID_CONFIGURATION:
            return "INVALID_CONFIGURATION";
        case LoadAvailabilityReason::RELAY_FAILURE:
            return "RELAY_FAILURE";
        default:
            return "UNKNOWN";
    }
}

static bool configureDefault(BestFirstSearch& search) {
    return search.configure(
        1.0F,  // w_P
        1.0F,  // w_S
        1.0F,  // w_B
        1.0F,  // w_Q
        1.0F,  // w_T
        10U,   // W_max
        20.0F, // SoC_min
        40.0F  // SoC_warn
    );
}

/**
 * Makes one load healthy, validates its per-load configuration, confirms that
 * it is an available AUTO load, then transfers only the documented candidate
 * fields into BestFirstSearch.
 */
static bool validateAndAddAutoLoad(BestFirstSearch& search,
                                   Load& load,
                                   std::size_t maximumBranches,
                                   std::uint16_t W_max) {
    load.setHealthy_i(true);

    if (!load.validateConfiguration(maximumBranches, W_max) ||
        !load.isCandidateAutoLoad()) {
        return false;
    }

    return search.addAutoLoad(
        load.getLoadId_i(),
        load.get_b_i(),
        load.get_P_i(),
        load.get_P_i_peak(),
        load.get_W_i(),
        load.get_a_i(),
        load.get_d_i());
}

static void printLoadRecord(const Load& load) {
    std::printf(
        "Load=%" PRIu32 " mode=%s node=%" PRIu32 " branch=%zu pin=%u "
        "P=%.2fW peak=%.2fW kappa=%.2f W_i=%u a=%u d=%u "
        "user=%s x=%s current=%s healthy=%u valid=%u available=%u reason=%s\n",
        load.getLoadId_i(),
        loadModeText(load.getMode_i()),
        load.getSmartNodeId_i(),
        load.get_b_i(),
        static_cast<unsigned int>(load.getRelayControlPin_i()),
        load.get_P_i(),
        load.get_P_i_peak(),
        load.get_kappa_i(),
        static_cast<unsigned int>(load.get_W_i()),
        static_cast<unsigned int>(load.get_a_i()),
        static_cast<unsigned int>(load.get_d_i()),
        loadStateText(load.getUserSelectedState_i()),
        loadStateText(load.get_x_i()),
        loadStateText(load.getCurrentState_i()),
        static_cast<unsigned int>(load.isHealthy()),
        static_cast<unsigned int>(load.isConfigurationValid()),
        static_cast<unsigned int>(load.isAvailable()),
        availabilityReasonText(load.getAvailabilityReason_i()));
}

/**
 * TEST 1
 * Unit tests for the Load data model and Candidate Preparation inputs.
 */
static void testLoadDataModel() {
    printSection("TEST 1 - LOAD DATA MODEL AND CANDIDATE PREPARATION");

    constexpr std::size_t maximumBranches = 2U;
    constexpr std::uint16_t W_max = 10U;

    Load autoLoad(
        101U, LoadMode::AUTO, 1U, 0U, 16U,
        20.0F, 30.0F, 10U, false, false,
        LoadState::OFF, LoadState::OFF, LoadState::OFF);

    reportCheck("Constructor derives kappa_i = P_i_peak / P_i",
                nearValue(autoLoad.get_kappa_i(), 1.5F));
    reportCheck("A new load requires health/configuration validation",
                !autoLoad.isAvailable() && !autoLoad.isConfigurationValid());

    autoLoad.setHealthy_i(true);
    reportCheck("Healthy but not-yet-validated load is still unavailable",
                autoLoad.isHealthy() && !autoLoad.isAvailable());

    const bool autoValid = autoLoad.validateConfiguration(maximumBranches, W_max);
    reportCheck("Valid AUTO load passes per-load validation", autoValid);
    reportCheck("Healthy + valid AUTO load is available", autoLoad.isAvailable());
    reportCheck("Healthy + valid AUTO load is a Best-First candidate",
                autoLoad.isCandidateAutoLoad());

    const bool surgeSet = autoLoad.setPowerProfileFromSurgeMultiplier_i(12.0F, 1.5F);
    reportCheck("Equation 4.17 surge multiplier is accepted", surgeSet);
    reportCheck("Equation 4.17 calculates P_i_peak = kappa_i * P_i",
                nearValue(autoLoad.get_P_i_peak(), 18.0F) &&
                nearValue(autoLoad.get_kappa_i(), 1.5F));
    reportCheck("Changing configuration invalidates prior validation",
                !autoLoad.isConfigurationValid());
    reportCheck("Revalidation restores a healthy valid load",
                autoLoad.validateConfiguration(maximumBranches, W_max) &&
                autoLoad.isAvailable());

    autoLoad.setHealthy_i(false);
    reportCheck("Hardware fault makes load unavailable",
                !autoLoad.isAvailable() &&
                autoLoad.getAvailabilityReason_i() ==
                    LoadAvailabilityReason::HARDWARE_FAULT);

    autoLoad.setHealthy_i(true);
    reportCheck("Health restoration clears hardware fault after valid config",
                autoLoad.isAvailable() &&
                autoLoad.getAvailabilityReason_i() ==
                    LoadAvailabilityReason::NONE);

    const bool relayFailureStored = autoLoad.setAvailabilityReason_i(
        LoadAvailabilityReason::RELAY_FAILURE);
    reportCheck("Relay failure can mark an otherwise valid load unavailable",
                relayFailureStored && !autoLoad.isAvailable() &&
                autoLoad.getAvailabilityReason_i() ==
                    LoadAvailabilityReason::RELAY_FAILURE);

    const bool availabilityCleared = autoLoad.setAvailabilityReason_i(
        LoadAvailabilityReason::NONE);
    reportCheck("Available state can be restored after relay fault is cleared",
                availabilityCleared && autoLoad.isAvailable());

    Load fixedLoad(
        201U, LoadMode::FIXED, 2U, 1U, 17U,
        15.0F, 15.0F, 8U, true, false,
        LoadState::ON, LoadState::OFF, LoadState::OFF);
    fixedLoad.setHealthy_i(true);

    reportCheck("FIXED load may retain valid schedule information",
                fixedLoad.validateConfiguration(maximumBranches, W_max));
    reportCheck("FIXED load is not an AUTO candidate",
                !fixedLoad.isCandidateAutoLoad());
    reportCheck("FIXED user-selected state can be applied to x_i",
                fixedLoad.set_x_i(fixedLoad.getUserSelectedState_i()) &&
                fixedLoad.get_x_i() == LoadState::ON);

    Load invalidSchedule(
        301U, LoadMode::AUTO, 1U, 0U, 18U,
        5.0F, 5.0F, 5U, false, true,
        LoadState::OFF, LoadState::OFF, LoadState::OFF);
    invalidSchedule.setHealthy_i(true);
    reportCheck("d_i=true while a_i=false fails Load validation",
                !invalidSchedule.validateConfiguration(maximumBranches, W_max));

    Load invalidBranch(
        302U, LoadMode::AUTO, 1U, 2U, 19U,
        5.0F, 5.0F, 5U, false, false,
        LoadState::OFF, LoadState::OFF, LoadState::OFF);
    invalidBranch.setHealthy_i(true);
    reportCheck("Branch index outside maximumBranches fails Load validation",
                !invalidBranch.validateConfiguration(maximumBranches, W_max));

    Load invalidPriority(
        303U, LoadMode::AUTO, 1U, 0U, 20U,
        5.0F, 5.0F, 11U, false, false,
        LoadState::OFF, LoadState::OFF, LoadState::OFF);
    invalidPriority.setHealthy_i(true);
    reportCheck("W_i above W_max fails Load validation",
                !invalidPriority.validateConfiguration(maximumBranches, W_max));

    Load invalidPeak(
        304U, LoadMode::AUTO, 1U, 0U, 21U,
        10.0F, 5.0F, 5U, false, false,
        LoadState::OFF, LoadState::OFF, LoadState::OFF);
    invalidPeak.setHealthy_i(true);
    reportCheck("P_i_peak below P_i fails Load validation",
                !invalidPeak.validateConfiguration(maximumBranches, W_max));

    std::printf("\nRepresentative records after unit tests:\n");
    printLoadRecord(autoLoad);
    printLoadRecord(fixedLoad);
}

/**
 * TEST 2
 * Integration test for Algorithm 4.2 Candidate Preparation followed by
 * Algorithms 4.3-4.5. FIXED loads form the boundary condition; only validated
 * available AUTO loads are transferred to BestFirstSearch.
 */
static void testLoadToBestFirstIntegration() {
    printSection("TEST 2 - LOAD -> BESTFIRSTSEARCH INTEGRATION");
    std::printf("Reference: Candidate Preparation + Algorithms 4.3-4.5\n");

    constexpr std::size_t maximumBranches = 2U;
    constexpr std::uint16_t W_max = 10U;
    constexpr float SoC = 30.0F;
    constexpr float P_available = 100.0F;
    constexpr float P_battery_max = 200.0F;
    constexpr float V_B = 10.0F;
    constexpr float I_main_max = 20.0F;

    Load fixedLoad(
        100U, LoadMode::FIXED, 1U, 0U, 15U,
        20.0F, 20.0F, 10U, false, false,
        LoadState::ON, LoadState::OFF, LoadState::ON);
    fixedLoad.setHealthy_i(true);

    const bool fixedValid = fixedLoad.validateConfiguration(maximumBranches, W_max);
    const bool fixedStateApplied = fixedValid &&
        fixedLoad.set_x_i(fixedLoad.getUserSelectedState_i());

    float initialBranch0 = 0.0F;
    float initialBranch1 = 0.0F;
    float P_fixed = 0.0F;

    if (fixedStateApplied && fixedLoad.get_x_i() == LoadState::ON) {
        P_fixed += fixedLoad.get_P_i();
        if (fixedLoad.get_b_i() == 0U) {
            initialBranch0 += fixedLoad.get_P_i();
        } else if (fixedLoad.get_b_i() == 1U) {
            initialBranch1 += fixedLoad.get_P_i();
        }
    }

    const float initialPRemaining = std::max(0.0F, P_available - P_fixed);
    const float initialPCommitted = P_fixed;

    BestFirstSearch search(3U, maximumBranches);
    const bool configured = configureDefault(search);
    const bool cycleStarted = configured && search.startPlanningCycle(
        SoC,
        P_available,
        initialPRemaining,
        initialPCommitted,
        P_battery_max,
        V_B,
        I_main_max);
    const bool branch0Set = cycleStarted &&
        search.setBranchState(0U, initialBranch0, 10.0F);
    const bool branch1Set = branch0Set &&
        search.setBranchState(1U, initialBranch1, 10.0F);

    Load load101(
        101U, LoadMode::AUTO, 1U, 0U, 16U,
        20.0F, 30.0F, 10U, false, false,
        LoadState::OFF, LoadState::OFF, LoadState::OFF);
    Load load102(
        102U, LoadMode::AUTO, 1U, 0U, 17U,
        10.0F, 10.0F, 5U, true, false,
        LoadState::OFF, LoadState::OFF, LoadState::OFF);
    Load load103(
        103U, LoadMode::AUTO, 2U, 1U, 18U,
        5.0F, 5.0F, 10U, false, false,
        LoadState::OFF, LoadState::OFF, LoadState::OFF);

    const bool load101Added = branch1Set &&
        validateAndAddAutoLoad(search, load101, maximumBranches, W_max);
    const bool load102Added = load101Added &&
        validateAndAddAutoLoad(search, load102, maximumBranches, W_max);
    const bool load103Added = load102Added &&
        validateAndAddAutoLoad(search, load103, maximumBranches, W_max);
    const bool schedulerCompleted = load103Added && search.run();

    const bool prerequisitesPassed = fixedValid && fixedStateApplied && configured &&
        cycleStarted && branch0Set && branch1Set && load101Added && load102Added &&
        load103Added && schedulerCompleted;
    if (!reportCheck("FIXED boundary, AUTO candidate preparation, and scheduler run",
                     prerequisitesPassed)) {
        return;
    }

    reportCheck("FIXED load remains outside BestFirstSearch candidate array",
                search.getAutoLoadCount() == 3U);
    reportCheck("P_fixed is committed before AUTO scheduling",
                nearValue(initialPCommitted, 20.0F) &&
                nearValue(initialPRemaining, 80.0F));

    std::printf("\nFIXED boundary load\n");
    printLoadRecord(fixedLoad);
    std::printf("P_fixed=%.2f W, initial P_remaining=%.2f W\n",
                P_fixed, initialPRemaining);

    constexpr float expectedB = 0.50F;
    reportCheck("Battery stress B is calculated from SoC_warn, SoC, SoC_min",
                nearValue(search.get_B(), expectedB));

    constexpr float expected_p_i[] = {0.20F, 0.10F, 0.05F};
    constexpr float expected_s_i[] = {0.10F, 0.00F, 0.00F};
    constexpr float expected_q_i[] = {1.00F, 0.50F, 1.00F};
    constexpr float expected_r_i[] = {0.00F, 1.00F, 0.00F};
    constexpr float expected_g_i[] = {0.80F, 0.60F, 0.55F};
    constexpr float expected_h_i[] = {0.00F, 1.50F, 0.00F};
    constexpr float expected_f_i[] = {0.80F, 2.10F, 0.55F};
    constexpr std::size_t expectedOrder[] = {1U, 2U, 0U};

    std::printf("\nCandidate Evaluation\n");
    std::printf("%-7s %-7s %-7s %-7s %-7s %-7s %-7s %-7s %-7s %s\n",
                "Load", "p", "s", "q", "r", "g", "h", "f", "Order", "Result");
    for (std::size_t i = 0U; i < 3U; ++i) {
        const bool rowPassed =
            nearValue(search.get_p_i(i), expected_p_i[i]) &&
            nearValue(search.get_s_i(i), expected_s_i[i]) &&
            nearValue(search.get_q_i(i), expected_q_i[i]) &&
            nearValue(search.get_r_i(i), expected_r_i[i]) &&
            nearValue(search.get_g_i(i), expected_g_i[i]) &&
            nearValue(search.get_h_i(i), expected_h_i[i]) &&
            nearValue(search.get_f_i(i), expected_f_i[i]) &&
            search.getProcessingOrder_i(i) == expectedOrder[i];

        std::printf("%-7" PRIu32 " %-7.2f %-7.2f %-7.2f %-7.2f %-7.2f %-7.2f %-7.2f %-7zu %s\n",
                    search.getAutoLoadId_i(i),
                    search.get_p_i(i),
                    search.get_s_i(i),
                    search.get_q_i(i),
                    search.get_r_i(i),
                    search.get_g_i(i),
                    search.get_h_i(i),
                    search.get_f_i(i),
                    search.getProcessingOrder_i(i),
                    recordResult(rowPassed));
    }

    for (std::size_t i = 0U; i < 3U; ++i) {
        const bool selected = search.get_x_i(i) == 1U &&
            search.getRejectionReason_i(i) == BestFirstSearch::NONE;
        char checkName[80] = {};
        std::snprintf(checkName, sizeof(checkName),
                      "AUTO load %" PRIu32 " is admitted with reason NONE",
                      search.getAutoLoadId_i(i));
        reportCheck(checkName, selected);
    }

    reportCheck("Final P_remaining includes FIXED + admitted AUTO loads",
                nearValue(search.get_P_remaining(), 45.0F));
    reportCheck("Final P_committed includes FIXED + admitted AUTO loads",
                nearValue(search.get_P_committed(), 55.0F));
    reportCheck("Final branch 0 committed power is 50 W",
                nearValue(search.get_P_b_i(0U), 50.0F));
    reportCheck("Final branch 1 committed power is 5 W",
                nearValue(search.get_P_b_i(1U), 5.0F));
}

/**
 * Executes one Algorithm 4.4 rejection case using a real Load object.
 */
static void runConstraintGuardCase(
    const char* caseName,
    float SoC,
    float P_available,
    float P_remaining,
    float P_committed,
    float P_battery_max,
    float V_B,
    float I_main_max,
    float P_branch,
    float I_branch_max,
    float P_i,
    float P_i_peak,
    std::uint8_t expectedReason,
    const char* expectedReasonText) {

    BestFirstSearch search(1U, 1U);
    const bool configured = configureDefault(search);
    const bool cycleStarted = configured && search.startPlanningCycle(
        SoC, P_available, P_remaining, P_committed,
        P_battery_max, V_B, I_main_max);
    const bool branchSet = cycleStarted &&
        search.setBranchState(0U, P_branch, I_branch_max);

    Load candidate(
        1U, LoadMode::AUTO, 1U, 0U, 16U,
        P_i, P_i_peak, 10U, false, false,
        LoadState::OFF, LoadState::OFF, LoadState::OFF);

    const bool loadAdded = branchSet &&
        validateAndAddAutoLoad(search, candidate, 1U, 10U);
    const bool runCompleted = loadAdded && search.run();

    const bool decisionPassed = configured && cycleStarted && branchSet &&
        loadAdded && runCompleted && search.get_x_i(0U) == 0U &&
        search.getRejectionReason_i(0U) == expectedReason;

    std::printf("\nCase: %s\n", caseName);
    printLoadRecord(candidate);
    std::printf("Expected reason: %-28s Observed: %-28s Result: %s\n",
                expectedReasonText,
                runCompleted ? search.getRejectionReasonText_i(0U) : "RUN_FAILED",
                recordResult(decisionPassed));
}

/** TEST 3 - all five documented Constraint Guard rejection paths. */
static void testConstraintGuard() {
    printSection("TEST 3 - CONSTRAINT GUARD REJECTIONS USING LOAD OBJECTS");

    runConstraintGuardCase(
        "LOW_BATTERY",
        20.0F, 0.0F, 0.0F, 0.0F,
        100.0F, 10.0F, 10.0F,
        0.0F, 10.0F,
        1.0F, 1.0F,
        BestFirstSearch::LOW_BATTERY, "LOW_BATTERY");

    runConstraintGuardCase(
        "POWER_BUDGET_EXCEEDED",
        80.0F, 100.0F, 5.0F, 0.0F,
        100.0F, 10.0F, 10.0F,
        0.0F, 10.0F,
        10.0F, 10.0F,
        BestFirstSearch::POWER_BUDGET_EXCEEDED,
        "POWER_BUDGET_EXCEEDED");

    runConstraintGuardCase(
        "BATTERY_CURRENT_LIMIT",
        80.0F, 100.0F, 80.0F, 20.0F,
        25.0F, 10.0F, 10.0F,
        0.0F, 10.0F,
        1.0F, 10.0F,
        BestFirstSearch::BATTERY_CURRENT_LIMIT,
        "BATTERY_CURRENT_LIMIT");

    runConstraintGuardCase(
        "MAIN_LIMIT_EXCEEDED",
        80.0F, 100.0F, 100.0F, 0.0F,
        100.0F, 10.0F, 1.0F,
        0.0F, 10.0F,
        1.0F, 20.0F,
        BestFirstSearch::MAIN_LIMIT_EXCEEDED,
        "MAIN_LIMIT_EXCEEDED");

    runConstraintGuardCase(
        "BRANCH_LIMIT_EXCEEDED",
        80.0F, 100.0F, 100.0F, 0.0F,
        100.0F, 10.0F, 10.0F,
        0.0F, 1.0F,
        1.0F, 20.0F,
        BestFirstSearch::BRANCH_LIMIT_EXCEEDED,
        "BRANCH_LIMIT_EXCEEDED");
}

/** TEST 4 - user priority changes ranking but never bypasses feasibility. */
static void testUserPriorityAllocation() {
    printSection("TEST 4 - USER PRIORITY ALLOCATION THROUGH LOAD OBJECTS");

    BestFirstSearch search(2U, 1U);
    const bool configured = search.configure(
        0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
        10U, 20.0F, 40.0F);
    const bool cycleStarted = configured && search.startPlanningCycle(
        80.0F, 10.0F, 10.0F, 0.0F, 100.0F, 10.0F, 10.0F);
    const bool branchSet = cycleStarted &&
        search.setBranchState(0U, 0.0F, 10.0F);

    Load highPriority(
        10U, LoadMode::AUTO, 1U, 0U, 16U,
        10.0F, 10.0F, 10U, false, false,
        LoadState::OFF, LoadState::OFF, LoadState::OFF);
    Load lowPriority(
        20U, LoadMode::AUTO, 1U, 0U, 17U,
        10.0F, 10.0F, 1U, false, false,
        LoadState::OFF, LoadState::OFF, LoadState::OFF);

    const bool highAdded = branchSet &&
        validateAndAddAutoLoad(search, highPriority, 1U, 10U);
    const bool lowAdded = highAdded &&
        validateAndAddAutoLoad(search, lowPriority, 1U, 10U);
    const bool runCompleted = lowAdded && search.run();

    if (!reportCheck("Priority test setup and scheduling complete",
                     configured && cycleStarted && branchSet &&
                     highAdded && lowAdded && runCompleted)) {
        return;
    }

    reportCheck("High priority has q_i=1 and f_i=0",
                nearValue(search.get_q_i(0U), 1.0F) &&
                nearValue(search.get_f_i(0U), 0.0F));
    reportCheck("Low priority has q_i=0.1 and f_i=0.9",
                nearValue(search.get_q_i(1U), 0.1F) &&
                nearValue(search.get_f_i(1U), 0.9F));
    reportCheck("High-priority load is processed first and admitted",
                search.getProcessingOrder_i(0U) == 0U &&
                search.get_x_i(0U) == 1U);
    reportCheck("Low-priority load cannot bypass exhausted power budget",
                search.getProcessingOrder_i(1U) == 1U &&
                search.get_x_i(1U) == 0U &&
                search.getRejectionReason_i(1U) ==
                    BestFirstSearch::POWER_BUDGET_EXCEEDED);
}

/** TEST 5 - branch totals are updated after every admitted candidate. */
static void testIncrementalBranchConstraint() {
    printSection("TEST 5 - INCREMENTAL BRANCH CONSTRAINT");

    BestFirstSearch search(3U, 1U);
    const bool configured = search.configure(
        0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
        10U, 20.0F, 40.0F);
    const bool cycleStarted = configured && search.startPlanningCycle(
        80.0F, 100.0F, 100.0F, 0.0F, 200.0F, 10.0F, 20.0F);
    const bool branchSet = cycleStarted &&
        search.setBranchState(0U, 0.0F, 3.0F);

    Load load1(1U, LoadMode::AUTO, 1U, 0U, 16U,
               10.0F, 20.0F, 10U, false, false,
               LoadState::OFF, LoadState::OFF, LoadState::OFF);
    Load load2(2U, LoadMode::AUTO, 1U, 0U, 17U,
               10.0F, 20.0F, 9U, false, false,
               LoadState::OFF, LoadState::OFF, LoadState::OFF);
    Load load3(3U, LoadMode::AUTO, 1U, 0U, 18U,
               10.0F, 20.0F, 8U, false, false,
               LoadState::OFF, LoadState::OFF, LoadState::OFF);

    const bool load1Added = branchSet &&
        validateAndAddAutoLoad(search, load1, 1U, 10U);
    const bool load2Added = load1Added &&
        validateAndAddAutoLoad(search, load2, 1U, 10U);
    const bool load3Added = load2Added &&
        validateAndAddAutoLoad(search, load3, 1U, 10U);
    const bool runCompleted = load3Added && search.run();

    if (!reportCheck("Incremental branch test setup and run",
                     configured && cycleStarted && branchSet &&
                     load1Added && load2Added && load3Added && runCompleted)) {
        return;
    }

    float expectedBranchPower = 0.0F;
    for (std::size_t order = 0U; order < 3U; ++order) {
        std::size_t candidate = search.getAutoLoadCount();
        for (std::size_t i = 0U; i < search.getAutoLoadCount(); ++i) {
            if (search.getProcessingOrder_i(i) == order) {
                candidate = i;
                break;
            }
        }

        if (candidate >= search.getAutoLoadCount()) {
            reportCheck("Every candidate receives one processing order", false);
            continue;
        }

        const float startupCurrent =
            (expectedBranchPower + search.get_P_i_peak(candidate)) /
            search.get_V_B();
        const bool expectedSelected =
            startupCurrent <= search.get_I_b_max_i(0U);
        const std::uint8_t expectedX = expectedSelected ? 1U : 0U;
        const std::uint8_t expectedReason = expectedSelected
            ? BestFirstSearch::NONE
            : BestFirstSearch::BRANCH_LIMIT_EXCEEDED;

        char checkName[96] = {};
        std::snprintf(checkName, sizeof(checkName),
                      "Order %zu load %" PRIu32 " branch feasibility result",
                      order, search.getAutoLoadId_i(candidate));
        reportCheck(checkName,
                    search.get_x_i(candidate) == expectedX &&
                    search.getRejectionReason_i(candidate) == expectedReason);

        if (expectedSelected) {
            expectedBranchPower += search.get_P_i(candidate);
        }
    }

    reportCheck("Final branch committed power is updated incrementally to 20 W",
                nearValue(expectedBranchPower, 20.0F) &&
                nearValue(search.get_P_b_i(0U), 20.0F));
}

static void printValidationResult(const char* condition, bool passed) {
    std::printf("%-66s %s\n", condition, recordResult(passed));
}

/**
 * TEST 6
 * Checks input validation and verifies that equal f_i values are not assigned
 * an undocumented secondary priority rule. Either equal-score order is valid;
 * both candidates must simply be processed exactly once.
 */
static void testValidationAndEqualScores() {
    printSection("TEST 6 - INPUT VALIDATION AND EQUAL f_i BEHAVIOUR");

    BestFirstSearch equalSearch(2U, 1U);
    bool operation = equalSearch.configure(
        0.0F, 0.0F, 0.0F, 0.0F, 0.0F,
        10U, 20.0F, 40.0F);
    operation = operation && equalSearch.startPlanningCycle(
        80.0F, 100.0F, 100.0F, 0.0F, 200.0F, 10.0F, 20.0F);
    operation = operation && equalSearch.setBranchState(0U, 0.0F, 20.0F);

    Load equalA(20U, LoadMode::AUTO, 1U, 0U, 16U,
                5.0F, 5.0F, 5U, false, false,
                LoadState::OFF, LoadState::OFF, LoadState::OFF);
    Load equalB(10U, LoadMode::AUTO, 1U, 0U, 17U,
                5.0F, 5.0F, 5U, false, false,
                LoadState::OFF, LoadState::OFF, LoadState::OFF);

    operation = operation &&
        validateAndAddAutoLoad(equalSearch, equalA, 1U, 10U);
    operation = operation &&
        validateAndAddAutoLoad(equalSearch, equalB, 1U, 10U);
    operation = operation && equalSearch.run();

    const std::size_t order0 = equalSearch.getProcessingOrder_i(0U);
    const std::size_t order1 = equalSearch.getProcessingOrder_i(1U);
    reportCheck("Equal-score candidates have equal f_i",
                operation && nearValue(equalSearch.get_f_i(0U),
                                       equalSearch.get_f_i(1U)));
    reportCheck("Equal-score candidates are both processed exactly once",
                operation && order0 < 2U && order1 < 2U && order0 != order1);
    reportCheck("No tie-order assumption is required for correct admission",
                operation && equalSearch.get_x_i(0U) == 1U &&
                equalSearch.get_x_i(1U) == 1U);

    BestFirstSearch invalidCapacity(0U, 1U);
    printValidationResult(
        "Zero AUTO-load capacity is rejected",
        !invalidCapacity.configure(1.0F, 1.0F, 1.0F, 1.0F, 1.0F,
                                   10U, 20.0F, 40.0F));

    BestFirstSearch search(2U, 1U);
    printValidationResult(
        "Negative scoring weight is rejected",
        !search.configure(-1.0F, 1.0F, 1.0F, 1.0F, 1.0F,
                          10U, 20.0F, 40.0F));
    printValidationResult(
        "SoC_warn equal to SoC_min is rejected",
        !search.configure(1.0F, 1.0F, 1.0F, 1.0F, 1.0F,
                          10U, 40.0F, 40.0F));

    const bool validConfiguration = configureDefault(search);
    const bool validCycle = validConfiguration && search.startPlanningCycle(
        80.0F, 100.0F, 100.0F, 0.0F, 100.0F, 10.0F, 10.0F);
    const bool validBranch = validCycle &&
        search.setBranchState(0U, 0.0F, 10.0F);

    Load first(1U, LoadMode::AUTO, 1U, 0U, 16U,
               10.0F, 10.0F, 10U, false, false,
               LoadState::OFF, LoadState::OFF, LoadState::OFF);
    const bool firstLoad = validBranch &&
        validateAndAddAutoLoad(search, first, 1U, 10U);
    printValidationResult("Valid Load candidate is accepted", firstLoad);

    Load duplicate(1U, LoadMode::AUTO, 1U, 0U, 17U,
                   10.0F, 10.0F, 10U, false, false,
                   LoadState::OFF, LoadState::OFF, LoadState::OFF);
    duplicate.setHealthy_i(true);
    const bool duplicateValid = duplicate.validateConfiguration(1U, 10U);
    printValidationResult(
        "Duplicate AUTO-load identifier is rejected by BestFirstSearch",
        duplicateValid &&
        !search.addAutoLoad(duplicate.getLoadId_i(), duplicate.get_b_i(),
                            duplicate.get_P_i(), duplicate.get_P_i_peak(),
                            duplicate.get_W_i(), duplicate.get_a_i(),
                            duplicate.get_d_i()));

    Load invalidSchedule(2U, LoadMode::AUTO, 1U, 0U, 18U,
                         10.0F, 10.0F, 10U, false, true,
                         LoadState::OFF, LoadState::OFF, LoadState::OFF);
    invalidSchedule.setHealthy_i(true);
    printValidationResult(
        "Invalid a_i=false, d_i=true is rejected before candidate registration",
        !invalidSchedule.validateConfiguration(1U, 10U));

    search.resetPlanningCycle();
    const bool resetPassed = search.isConfigured() &&
                             !search.isPlanningCycleStarted() &&
                             search.getAutoLoadCount() == 0U;
    printValidationResult("Reset retains policy and clears cycle data", resetPassed);

    const bool emptyCycleStarted = search.startPlanningCycle(
        80.0F, 100.0F, 100.0F, 0.0F, 100.0F, 10.0F, 10.0F);
    const bool emptyCycleRun = emptyCycleStarted && search.run();
    printValidationResult(
        "Valid empty planning cycle completes",
        emptyCycleRun && search.getSelectedAutoLoadCount() == 0U &&
        search.getRejectedAutoLoadCount() == 0U);

    search.resetPlanningCycle();
    const bool missingBranchCycle = search.startPlanningCycle(
        80.0F, 100.0F, 100.0F, 0.0F, 100.0F, 10.0F, 10.0F);
    Load missingBranchLoad(5U, LoadMode::AUTO, 1U, 0U, 19U,
                           10.0F, 10.0F, 10U, false, false,
                           LoadState::OFF, LoadState::OFF, LoadState::OFF);
    const bool candidateAdded = missingBranchCycle &&
        validateAndAddAutoLoad(search, missingBranchLoad, 1U, 10U);
    printValidationResult(
        "Scheduler refuses candidate whose branch state is missing",
        candidateAdded && !search.run());
}

static float expectedScalabilityCommittedPower(std::size_t loadCount) {
    float total = 0.0F;
    for (std::size_t i = 0U; i < loadCount; ++i) {
        total += 1.0F + static_cast<float>((i * 37U) % 10U) * 0.05F;
    }
    return total;
}

/**
 * Executes one on-device scalability cycle. Registration time includes Load
 * construction, Load health/configuration validation, and transfer into
 * BestFirstSearch; scheduler time measures BestFirstSearch::run() only.
 */
static bool executeScalabilityCycle(
    std::size_t loadCount,
    std::int64_t& setupMicroseconds,
    std::int64_t& registrationMicroseconds,
    std::int64_t& schedulerMicroseconds,
    std::int64_t& totalMicroseconds,
    float& observedCommittedPower,
    float& observedRemainingPower,
    std::size_t& selectedLoads,
    std::size_t& rejectedLoads,
    std::size_t& freeHeapDuringCycle) {

    const std::int64_t totalStart = esp_timer_get_time();
    BestFirstSearch search(loadCount, 1U);

    bool valid = search.configure(
        1.0F, 1.0F, 1.0F, 1.0F, 1.0F,
        100U, 20.0F, 40.0F);

    const float P_available = static_cast<float>(loadCount) * 2.0F;
    valid = valid && search.startPlanningCycle(
        80.0F,
        P_available,
        P_available,
        0.0F,
        P_available * 2.0F,
        12.0F,
        P_available);
    valid = valid && search.setBranchState(0U, 0.0F, P_available);
    const std::int64_t setupEnd = esp_timer_get_time();

    float expectedCommittedPower = 0.0F;
    const std::int64_t registrationStart = esp_timer_get_time();
    for (std::size_t i = 0U; valid && i < loadCount; ++i) {
        const float P_i =
            1.0F + static_cast<float>((i * 37U) % 10U) * 0.05F;
        const float P_i_peak =
            P_i + 0.5F + static_cast<float>((i * 17U) % 5U) * 0.05F;
        const std::uint16_t W_i =
            static_cast<std::uint16_t>(((i * 31U) % 100U) + 1U);
        const bool a_i = (i % 3U) == 0U;
        const bool d_i = (i % 6U) == 0U;

        Load load(
            static_cast<std::uint32_t>(i + 1U),
            LoadMode::AUTO,
            1U,
            0U,
            static_cast<std::uint8_t>(16U + (i % 8U)),
            P_i,
            P_i_peak,
            W_i,
            a_i,
            d_i,
            LoadState::OFF,
            LoadState::OFF,
            LoadState::OFF);

        valid = validateAndAddAutoLoad(search, load, 1U, 100U);
        if (valid) {
            expectedCommittedPower += P_i;
        }
    }
    const std::int64_t registrationEnd = esp_timer_get_time();
    freeHeapDuringCycle = heap_caps_get_free_size(MALLOC_CAP_8BIT);

    const std::int64_t schedulerStart = esp_timer_get_time();
    valid = valid && search.run();
    const std::int64_t schedulerEnd = esp_timer_get_time();
    freeHeapDuringCycle = std::min(
        freeHeapDuringCycle,
        heap_caps_get_free_size(MALLOC_CAP_8BIT));

    setupMicroseconds = setupEnd - totalStart;
    registrationMicroseconds = registrationEnd - registrationStart;
    schedulerMicroseconds = schedulerEnd - schedulerStart;
    totalMicroseconds = schedulerEnd - totalStart;
    observedCommittedPower = search.get_P_committed();
    observedRemainingPower = search.get_P_remaining();
    selectedLoads = search.getSelectedAutoLoadCount();
    rejectedLoads = search.getRejectedAutoLoadCount();

    bool resultsValid = valid && search.isPlanningCycleCompleted() &&
                        search.getAutoLoadCount() == loadCount &&
                        selectedLoads == loadCount && rejectedLoads == 0U &&
                        nearValue(observedCommittedPower,
                                  expectedCommittedPower, 0.5F) &&
                        nearValue(observedRemainingPower,
                                  P_available - expectedCommittedPower, 0.5F);

    for (std::size_t i = 0U; resultsValid && i < loadCount; ++i) {
        resultsValid = search.get_x_i(i) == 1U &&
                       search.getRejectionReason_i(i) == BestFirstSearch::NONE &&
                       std::isfinite(search.get_f_i(i));
    }

    return resultsValid;
}

static std::int64_t medianMicroseconds(std::int64_t* values,
                                       std::size_t count) {
    if (values == nullptr || count == 0U) {
        return 0;
    }

    std::sort(values, values + count);
    return values[count / 2U];
}

/** TEST 7 - real ESP32 timing and heap use with both classes in the path. */
static void testEsp32ScalabilityAndExecutionTime() {
    printSection("TEST 7 - ESP32 LOAD + BESTFIRSTSEARCH SCALABILITY");
    std::printf("Registration time includes Load construction/validation.\n");
    std::printf("Scheduler time measures BestFirstSearch::run() only.\n");

    constexpr std::size_t loadCounts[] = {10U, 100U, 200U, 500U, 1000U};
    constexpr std::size_t numberOfLoadCounts =
        sizeof(loadCounts) / sizeof(loadCounts[0]);
    constexpr std::size_t warmUpRuns = 3U;
    constexpr std::size_t measuredRuns = 21U;

    std::int64_t medianSetup[numberOfLoadCounts] = {};
    std::int64_t medianRegistration[numberOfLoadCounts] = {};
    std::int64_t medianScheduler[numberOfLoadCounts] = {};
    std::int64_t medianTotal[numberOfLoadCounts] = {};
    float observedCommitted[numberOfLoadCounts] = {};
    float observedRemaining[numberOfLoadCounts] = {};
    std::size_t selected[numberOfLoadCounts] = {};
    std::size_t rejected[numberOfLoadCounts] = {};
    std::size_t minimumFreeHeap[numberOfLoadCounts] = {};
    bool scalePassed[numberOfLoadCounts] = {};

    for (std::size_t scale = 0U; scale < numberOfLoadCounts; ++scale) {
        std::int64_t setupMicroseconds = 0;
        std::int64_t registrationMicroseconds = 0;
        std::int64_t schedulerMicroseconds = 0;
        std::int64_t totalMicroseconds = 0;
        std::size_t freeHeapDuringCycle = 0U;

        bool allExecutionsPassed = true;
        minimumFreeHeap[scale] = std::numeric_limits<std::size_t>::max();

        for (std::size_t run = 0U; run < warmUpRuns; ++run) {
            const bool runPassed = executeScalabilityCycle(
                loadCounts[scale],
                setupMicroseconds,
                registrationMicroseconds,
                schedulerMicroseconds,
                totalMicroseconds,
                observedCommitted[scale],
                observedRemaining[scale],
                selected[scale],
                rejected[scale],
                freeHeapDuringCycle);
            allExecutionsPassed = allExecutionsPassed && runPassed;
            minimumFreeHeap[scale] = std::min(
                minimumFreeHeap[scale], freeHeapDuringCycle);
            vTaskDelay(1U);
        }

        std::int64_t setupSamples[measuredRuns] = {};
        std::int64_t registrationSamples[measuredRuns] = {};
        std::int64_t schedulerSamples[measuredRuns] = {};
        std::int64_t totalSamples[measuredRuns] = {};

        for (std::size_t run = 0U; run < measuredRuns; ++run) {
            const bool runPassed = executeScalabilityCycle(
                loadCounts[scale],
                setupMicroseconds,
                registrationMicroseconds,
                schedulerMicroseconds,
                totalMicroseconds,
                observedCommitted[scale],
                observedRemaining[scale],
                selected[scale],
                rejected[scale],
                freeHeapDuringCycle);
            allExecutionsPassed = allExecutionsPassed && runPassed;
            setupSamples[run] = setupMicroseconds;
            registrationSamples[run] = registrationMicroseconds;
            schedulerSamples[run] = schedulerMicroseconds;
            totalSamples[run] = totalMicroseconds;
            minimumFreeHeap[scale] = std::min(
                minimumFreeHeap[scale], freeHeapDuringCycle);
            vTaskDelay(1U);
        }

        medianSetup[scale] = medianMicroseconds(setupSamples, measuredRuns);
        medianRegistration[scale] =
            medianMicroseconds(registrationSamples, measuredRuns);
        medianScheduler[scale] =
            medianMicroseconds(schedulerSamples, measuredRuns);
        medianTotal[scale] = medianMicroseconds(totalSamples, measuredRuns);
        scalePassed[scale] = allExecutionsPassed;
    }

    std::printf("\nPower and decision verification\n");
    std::printf("%-7s %-11s %-12s %-12s %-12s %-12s %-9s %-9s %s\n",
                "Loads", "P_avail", "Commit exp", "Commit obs", "Remain exp",
                "Remain obs", "Selected", "Rejected", "Result");

    for (std::size_t scale = 0U; scale < numberOfLoadCounts; ++scale) {
        const float P_available = static_cast<float>(loadCounts[scale]) * 2.0F;
        const float expectedCommitted =
            expectedScalabilityCommittedPower(loadCounts[scale]);
        const float expectedRemaining = P_available - expectedCommitted;
        const bool powerPassed = scalePassed[scale] &&
                                 nearValue(observedCommitted[scale],
                                           expectedCommitted, 0.5F) &&
                                 nearValue(observedRemaining[scale],
                                           expectedRemaining, 0.5F) &&
                                 selected[scale] == loadCounts[scale] &&
                                 rejected[scale] == 0U;
        std::printf("%-7zu %-11.2f %-12.2f %-12.2f %-12.2f %-12.2f %-9zu %-9zu %s\n",
                    loadCounts[scale],
                    P_available,
                    expectedCommitted,
                    observedCommitted[scale],
                    expectedRemaining,
                    observedRemaining[scale],
                    selected[scale],
                    rejected[scale],
                    recordResult(powerPassed));
    }

    std::printf("\nESP32 timing and memory findings\n");
    std::printf("Medians use %zu measured runs after %zu warm-up runs.\n\n",
                measuredRuns, warmUpRuns);
    std::printf("%-7s %-11s %-15s %-13s %-11s %-16s\n",
                "Loads", "Setup us", "Registration us", "Scheduler us",
                "Total us", "Min free heap B");

    for (std::size_t scale = 0U; scale < numberOfLoadCounts; ++scale) {
        std::printf("%-7zu %-11" PRId64 " %-15" PRId64 " %-13" PRId64
                    " %-11" PRId64 " %-16zu\n",
                    loadCounts[scale],
                    medianSetup[scale],
                    medianRegistration[scale],
                    medianScheduler[scale],
                    medianTotal[scale],
                    minimumFreeHeap[scale]);
    }
}

extern "C" void app_main(void) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    vTaskDelay(pdMS_TO_TICKS(1500U));

    esp_chip_info_t chipInfo{};
    esp_chip_info(&chipInfo);

    std::printf("\nKILOWATTS LOAD + BESTFIRSTSEARCH ESP32 UNIT TEST REPORT\n");
    std::printf("Author: Chalwe Silas\n");
    std::printf("Programme: Final-Year Computer Engineering\n");
    std::printf("Institution: The Copperbelt University\n");
    std::printf("Tests: Load data model, Candidate Preparation, Algorithms 4.3-4.5\n\n");
    std::printf("ESP-IDF version: %s\n", esp_get_idf_version());
    std::printf("Chip model code: %d\n", static_cast<int>(chipInfo.model));
    std::printf("CPU cores: %u\n", static_cast<unsigned int>(chipInfo.cores));
    std::printf("Silicon revision: %u\n", static_cast<unsigned int>(chipInfo.revision));
    std::printf("Free 8-bit heap before tests: %zu bytes\n",
                heap_caps_get_free_size(MALLOC_CAP_8BIT));
    std::printf("Total PSRAM: %zu bytes\n",
                heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    std::printf("Free PSRAM before tests: %zu bytes\n",
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    testLoadDataModel();
    testLoadToBestFirstIntegration();
    testConstraintGuard();
    testUserPriorityAllocation();
    testIncrementalBranchConstraint();
    testValidationAndEqualScores();
    testEsp32ScalabilityAndExecutionTime();

    printSection("FINAL ESP32 TEST SUMMARY");
    std::printf("Passed checks: %zu\n", passedChecks);
    std::printf("Failed checks: %zu\n", failedChecks);
    std::printf("Free 8-bit heap after tests: %zu bytes\n",
                heap_caps_get_free_size(MALLOC_CAP_8BIT));
    std::printf("Minimum free 8-bit heap observed: %zu bytes\n",
                heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    std::printf("Free PSRAM after tests: %zu bytes\n",
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    std::printf("OVERALL RESULT: %s\n",
                failedChecks == 0U ? "PASS" : "FAIL");
    std::printf("Load + BestFirstSearch ESP32 unit/integration tests complete.\n");
}