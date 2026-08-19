/**
 * @file test_node_lifecycle.cpp
 * @brief Host-native correctness tests for NodeLifecycle's transition
 *        rules.
 *
 * This file uses a standard host int main(), not an ESP-IDF app_main(), so
 * it can be compiled and run by run_cpp_test.sh's plain g++ invocation.
 */

#include "NodeLifecycle.h"

#include <cstdio>
#include <cstring>

using kilowatts::isValidNodeLifecycleTransition;
using kilowatts::NodeLifecycleState;
using kilowatts::NodeRole;
using kilowatts::toText;

namespace {

std::size_t passedChecks = 0U;
std::size_t failedChecks = 0U;

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

void testBootAndFirstCommissioningPath() {
    printSection("TEST 1 - BOOT AND FIRST COMMISSIONING PATH");

    reportCheck("FACTORY -> UNCOMMISSIONED is legal",
                isValidNodeLifecycleTransition(NodeLifecycleState::FACTORY, NodeLifecycleState::UNCOMMISSIONED));
    reportCheck("FACTORY -> DISCOVERED is illegal (must resolve through UNCOMMISSIONED first)",
                !isValidNodeLifecycleTransition(NodeLifecycleState::FACTORY, NodeLifecycleState::DISCOVERED));
    reportCheck("FACTORY -> COMMISSIONED is illegal",
                !isValidNodeLifecycleTransition(NodeLifecycleState::FACTORY, NodeLifecycleState::COMMISSIONED));

    reportCheck("UNCOMMISSIONED -> DISCOVERED is legal (Central records a real ESP-NOW identity report)",
                isValidNodeLifecycleTransition(NodeLifecycleState::UNCOMMISSIONED, NodeLifecycleState::DISCOVERED));
    reportCheck("UNCOMMISSIONED -> CONFIGURING is legal (a Node commissions itself directly, without a separate DISCOVERED step)",
                isValidNodeLifecycleTransition(NodeLifecycleState::UNCOMMISSIONED, NodeLifecycleState::CONFIGURING));
    reportCheck("DISCOVERED -> CONFIGURING is legal",
                isValidNodeLifecycleTransition(NodeLifecycleState::DISCOVERED, NodeLifecycleState::CONFIGURING));
    reportCheck("DISCOVERED -> COMMISSIONED is illegal (must pass through CONFIGURING)",
                !isValidNodeLifecycleTransition(NodeLifecycleState::DISCOVERED, NodeLifecycleState::COMMISSIONED));

    reportCheck("CONFIGURING -> COMMISSIONED is legal (successful commissioning)",
                isValidNodeLifecycleTransition(NodeLifecycleState::CONFIGURING, NodeLifecycleState::COMMISSIONED));
    reportCheck("CONFIGURING -> UNCOMMISSIONED is legal (a failed/timed-out commissioning attempt rolls back)",
                isValidNodeLifecycleTransition(NodeLifecycleState::CONFIGURING, NodeLifecycleState::UNCOMMISSIONED));
    reportCheck("CONFIGURING -> DISCOVERED is illegal",
                !isValidNodeLifecycleTransition(NodeLifecycleState::CONFIGURING, NodeLifecycleState::DISCOVERED));
}

void testOperationalAndDecommissioning() {
    printSection("TEST 2 - OPERATIONAL AND DECOMMISSIONING");

    reportCheck("COMMISSIONED -> OPERATIONAL is legal (reserved for a later Branch/Load phase)",
                isValidNodeLifecycleTransition(NodeLifecycleState::COMMISSIONED, NodeLifecycleState::OPERATIONAL));
    reportCheck("OPERATIONAL -> UNCOMMISSIONED is illegal (must go through DECOMMISSIONED)",
                !isValidNodeLifecycleTransition(NodeLifecycleState::OPERATIONAL, NodeLifecycleState::UNCOMMISSIONED));

    reportCheck("UNCOMMISSIONED -> DECOMMISSIONED is legal",
                isValidNodeLifecycleTransition(NodeLifecycleState::UNCOMMISSIONED, NodeLifecycleState::DECOMMISSIONED));
    reportCheck("DISCOVERED -> DECOMMISSIONED is legal",
                isValidNodeLifecycleTransition(NodeLifecycleState::DISCOVERED, NodeLifecycleState::DECOMMISSIONED));
    reportCheck("CONFIGURING -> DECOMMISSIONED is legal (abort mid-flight)",
                isValidNodeLifecycleTransition(NodeLifecycleState::CONFIGURING, NodeLifecycleState::DECOMMISSIONED));
    reportCheck("COMMISSIONED -> DECOMMISSIONED is legal",
                isValidNodeLifecycleTransition(NodeLifecycleState::COMMISSIONED, NodeLifecycleState::DECOMMISSIONED));
    reportCheck("OPERATIONAL -> DECOMMISSIONED is legal",
                isValidNodeLifecycleTransition(NodeLifecycleState::OPERATIONAL, NodeLifecycleState::DECOMMISSIONED));
    reportCheck("FACTORY -> DECOMMISSIONED is illegal (nothing has ever been commissioned yet)",
                !isValidNodeLifecycleTransition(NodeLifecycleState::FACTORY, NodeLifecycleState::DECOMMISSIONED));

    reportCheck("DECOMMISSIONED -> UNCOMMISSIONED is legal (re-entering the lifecycle)",
                isValidNodeLifecycleTransition(NodeLifecycleState::DECOMMISSIONED, NodeLifecycleState::UNCOMMISSIONED));
    reportCheck("DECOMMISSIONED -> COMMISSIONED is illegal (must be configured again, never restored directly)",
                !isValidNodeLifecycleTransition(NodeLifecycleState::DECOMMISSIONED, NodeLifecycleState::COMMISSIONED));
}

void testNoSelfTransitions() {
    printSection("TEST 3 - NO STATE VALIDLY TRANSITIONS TO ITSELF");

    const NodeLifecycleState allStates[] = {
        NodeLifecycleState::FACTORY, NodeLifecycleState::UNCOMMISSIONED,
        NodeLifecycleState::DISCOVERED, NodeLifecycleState::CONFIGURING,
        NodeLifecycleState::COMMISSIONED, NodeLifecycleState::OPERATIONAL,
        NodeLifecycleState::DECOMMISSIONED
    };

    bool allRejectedSelfTransition = true;
    for (NodeLifecycleState state : allStates) {
        if (isValidNodeLifecycleTransition(state, state)) {
            allRejectedSelfTransition = false;
        }
    }

    reportCheck("Every state rejects a transition to itself", allRejectedSelfTransition);
}

void testTextRepresentation() {
    printSection("TEST 4 - TEXT REPRESENTATION");

    reportCheck("toText(UNCOMMISSIONED) == \"uncommissioned\"",
                std::strcmp(toText(NodeLifecycleState::UNCOMMISSIONED), "uncommissioned") == 0);
    reportCheck("toText(OPERATIONAL) == \"operational\"",
                std::strcmp(toText(NodeLifecycleState::OPERATIONAL), "operational") == 0);
    reportCheck("toText(NodeRole::CENTRAL) == \"central\"",
                std::strcmp(toText(NodeRole::CENTRAL), "central") == 0);
    reportCheck("toText(NodeRole::SMART) == \"smart\"",
                std::strcmp(toText(NodeRole::SMART), "smart") == 0);
}

} // namespace

int main() {
    std::printf("KILOWATTS NODELIFECYCLE HOST TEST REPORT\n");

    testBootAndFirstCommissioningPath();
    testOperationalAndDecommissioning();
    testNoSelfTransitions();
    testTextRepresentation();

    printSection("FINAL TEST SUMMARY");
    std::printf("Passed checks: %zu\n", passedChecks);
    std::printf("Failed checks: %zu\n", failedChecks);
    std::printf("OVERALL RESULT: %s\n", failedChecks == 0U ? "PASS" : "FAIL");

    return failedChecks == 0U ? 0 : 1;
}
