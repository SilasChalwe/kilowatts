/**
 * @file test_node_identity_store.cpp
 * @brief Host-native correctness tests for NodeIdentityStore's local
 *        commissioning bookkeeping.
 *
 * NVS persistence (loadPersisted()/persist()) is ESP32-only; this file
 * confirms a host build honestly reports failure rather than fabricating
 * durable storage, and exercises everything else directly.
 *
 * This file uses a standard host int main(), not an ESP-IDF app_main(), so
 * it can be compiled and run by run_cpp_test.sh's plain g++ invocation.
 */

#include "NodeIdentityStore.h"

#include <cstdio>
#include <cstring>

using kilowatts::NodeIdentityStore;
using kilowatts::NodeLifecycleState;

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

void testDefaultState() {
    printSection("TEST 1 - DEFAULT STATE");

    NodeIdentityStore identity;
    reportCheck("A new store starts UNCOMMISSIONED",
                identity.getLifecycleState() == NodeLifecycleState::UNCOMMISSIONED);
    reportCheck("A new store's friendly name starts empty",
                std::strcmp(identity.getFriendlyName(), "") == 0);
}

void testFirstCommissioning() {
    printSection("TEST 2 - FIRST COMMISSIONING");

    NodeIdentityStore identity;

    reportCheck("applyCommission() with an empty name is rejected", !identity.applyCommission(""));
    reportCheck("applyCommission() with a null name is rejected", !identity.applyCommission(nullptr));
    reportCheck("applyCommission() with a name exceeding the buffer is rejected",
                !identity.applyCommission("This friendly name is far too long to fit in the buffer"));
    reportCheck("A rejected applyCommission() leaves the store UNCOMMISSIONED",
                identity.getLifecycleState() == NodeLifecycleState::UNCOMMISSIONED);

    reportCheck("applyCommission() with a valid name is accepted", identity.applyCommission("Sitting Room"));
    reportCheck("Accepted commissioning moves lifecycleState to COMMISSIONED",
                identity.getLifecycleState() == NodeLifecycleState::COMMISSIONED);
    reportCheck("Accepted commissioning commits the friendly name immediately",
                std::strcmp(identity.getFriendlyName(), "Sitting Room") == 0);
}

void testRename() {
    printSection("TEST 3 - RENAME AN ALREADY-COMMISSIONED NODE");

    NodeIdentityStore identity;
    identity.applyCommission("Sitting Room");

    reportCheck("applyCommission() (rename) on an already-COMMISSIONED store is accepted",
                identity.applyCommission("Living Room"));
    reportCheck("A rename does not disturb lifecycleState (stays COMMISSIONED)",
                identity.getLifecycleState() == NodeLifecycleState::COMMISSIONED);
    reportCheck("A rename updates the friendly name",
                std::strcmp(identity.getFriendlyName(), "Living Room") == 0);
}

void testDecommission() {
    printSection("TEST 4 - DECOMMISSION RESETS TO UNCOMMISSIONED");

    NodeIdentityStore identity;
    identity.applyCommission("Sitting Room");

    identity.applyDecommission();
    reportCheck("applyDecommission() resets lifecycleState to UNCOMMISSIONED (never DECOMMISSIONED)",
                identity.getLifecycleState() == NodeLifecycleState::UNCOMMISSIONED);
    reportCheck("applyDecommission() clears the friendly name",
                std::strcmp(identity.getFriendlyName(), "") == 0);

    reportCheck("A Node may be commissioned again after decommissioning",
                identity.applyCommission("Garage"));
    reportCheck("Re-commissioning after decommission moves to COMMISSIONED",
                identity.getLifecycleState() == NodeLifecycleState::COMMISSIONED);
}

void testPersistenceHonestOnHostBuild() {
    printSection("TEST 5 - PERSISTENCE IS HONEST ON A HOST BUILD");

    NodeIdentityStore identity;
    identity.applyCommission("Sitting Room");

    reportCheck("persist() on a host build reports failure, not a fabricated success", !identity.persist());

    NodeIdentityStore freshIdentity;
    reportCheck("loadPersisted() on a host build reports failure, not a fabricated restore",
                !freshIdentity.loadPersisted());
    reportCheck("A failed loadPersisted() leaves the store UNCOMMISSIONED",
                freshIdentity.getLifecycleState() == NodeLifecycleState::UNCOMMISSIONED);
}

} // namespace

int main() {
    std::printf("KILOWATTS NODEIDENTITYSTORE HOST TEST REPORT\n");

    testDefaultState();
    testFirstCommissioning();
    testRename();
    testDecommission();
    testPersistenceHonestOnHostBuild();

    printSection("FINAL TEST SUMMARY");
    std::printf("Passed checks: %zu\n", passedChecks);
    std::printf("Failed checks: %zu\n", failedChecks);
    std::printf("OVERALL RESULT: %s\n", failedChecks == 0U ? "PASS" : "FAIL");

    return failedChecks == 0U ? 0 : 1;
}
