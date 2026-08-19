/**
 * @file test_node_commissioning_registry.cpp
 * @brief Host-native correctness tests for NodeCommissioningRegistry's
 *        lookup/lifecycle bookkeeping.
 *
 * NVS persistence (loadPersisted()/persist()) is ESP32-only; this file
 * confirms a host build honestly reports failure rather than fabricating
 * durable storage, and exercises everything else directly.
 *
 * This file uses a standard host int main(), not an ESP-IDF app_main(), so
 * it can be compiled and run by run_cpp_test.sh's plain g++ invocation.
 */

#include "NodeCommissioningRegistry.h"

#include <cstdio>
#include <cstring>

using kilowatts::MacAddress;
using kilowatts::NodeCommissioningRegistry;
using kilowatts::NodeLifecycleState;
using kilowatts::NodeRole;

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

const MacAddress SMART_MAC = {0x24, 0x6F, 0x28, 0xAA, 0xBB, 0x01};
const MacAddress OTHER_MAC = {0x24, 0x6F, 0x28, 0xAA, 0xBB, 0x02};
const MacAddress CENTRAL_MAC = {0x24, 0x6F, 0x28, 0xAA, 0xBB, 0x03};

/*
 * The installer portal must receive only the GPIOs the flashed Smart Node
 * declared safe. This is a discovery fact and must refresh if a Node is
 * reflashed with a corrected board profile.
 */
void testRelayCapabilities() {
    printSection("TEST 1A - RELAY GPIO CAPABILITY INVENTORY");

    NodeCommissioningRegistry registry;
    const std::uint8_t firstProfile[] = {16U, 17U};
    registry.recordDiscovered(SMART_MAC, NodeRole::SMART, "0.2.1", "esp32:2core",
                              firstProfile, 2U, 1000U);

    const NodeCommissioningRegistry::CommissioningRecord* first = registry.findByMac(SMART_MAC);
    reportCheck("Discovery stores the number of declared safe relay GPIOs",
                first != nullptr && first->relayCapabilityCount == 2U);
    reportCheck("Discovery stores each declared safe relay GPIO in order",
                first != nullptr && first->relayPins[0] == 16U && first->relayPins[1] == 17U);

    const std::uint8_t correctedProfile[] = {18U};
    registry.recordDiscovered(SMART_MAC, NodeRole::SMART, "0.2.2", "esp32:2core",
                              correctedProfile, 1U, 2000U);
    const NodeCommissioningRegistry::CommissioningRecord* refreshed = registry.findByMac(SMART_MAC);
    reportCheck("A fresh identity report replaces stale relay capabilities",
                refreshed != nullptr && refreshed->relayCapabilityCount == 1U &&
                refreshed->relayPins[0] == 18U && refreshed->relayPins[1] == 0U);
}

void testDiscovery() {
    printSection("TEST 1 - DISCOVERY CREATES/REFRESHES A RECORD");

    NodeCommissioningRegistry registry;
    reportCheck("A new registry starts with zero records", registry.getCount() == 0U);

    reportCheck("recordDiscovered() for a new MAC returns true (new record)",
                registry.recordDiscovered(SMART_MAC, NodeRole::SMART, "0.2.0-foundation", "esp32:2core", 1000U));
    reportCheck("getCount() becomes 1", registry.getCount() == 1U);

    const NodeCommissioningRegistry::CommissioningRecord* record = registry.findByMac(SMART_MAC);
    reportCheck("findByMac() finds the new record",
                record != nullptr && record->lifecycleState == NodeLifecycleState::UNCOMMISSIONED &&
                record->role == NodeRole::SMART);
    reportCheck("A new record's friendlyName starts empty",
                record != nullptr && record->friendlyName[0] == '\0');

    reportCheck("recordDiscovered() for the same MAC again returns false (not new)",
                !registry.recordDiscovered(SMART_MAC, NodeRole::SMART, "0.2.1", "esp32:2core", 2000U));
    reportCheck("getCount() stays 1 (upsert, not duplicate)", registry.getCount() == 1U);

    const NodeCommissioningRegistry::CommissioningRecord* refreshed = registry.findByMac(SMART_MAC);
    reportCheck("Re-discovery refreshes firmwareVersion",
                refreshed != nullptr && std::strcmp(refreshed->firmwareVersion, "0.2.1") == 0);
    reportCheck("Re-discovery refreshes discoveredAtMilliseconds (last-seen)",
                refreshed != nullptr && refreshed->discoveredAtMilliseconds == 2000U);
    reportCheck("Re-discovery of an UNCOMMISSIONED record leaves lifecycleState unchanged",
                refreshed != nullptr && refreshed->lifecycleState == NodeLifecycleState::UNCOMMISSIONED);

    reportCheck("findByMac() returns nullptr for an unknown MAC", registry.findByMac(OTHER_MAC) == nullptr);
}

void testRegisterSelf() {
    printSection("TEST 2 - CENTRAL SELF-REGISTRATION");

    NodeCommissioningRegistry registry;
    reportCheck("registerSelf() on an unknown MAC returns true",
                registry.registerSelf(CENTRAL_MAC, NodeRole::CENTRAL, "Central", "0.2.0-foundation", "esp32:2core",
                                       nullptr, 0U, 500U));

    const NodeCommissioningRegistry::CommissioningRecord* record = registry.findByMac(CENTRAL_MAC);
    reportCheck("Central self-registers directly as COMMISSIONED",
                record != nullptr && record->lifecycleState == NodeLifecycleState::COMMISSIONED);
    reportCheck("Central self-registers with its friendly name already set",
                record != nullptr && std::strcmp(record->friendlyName, "Central") == 0);

    reportCheck("registerSelf() a second time for the same MAC is a no-op (returns false)",
                !registry.registerSelf(CENTRAL_MAC, NodeRole::CENTRAL, "Renamed", "0.2.0-foundation", "esp32:2core",
                                        nullptr, 0U, 600U));
    const NodeCommissioningRegistry::CommissioningRecord* unchanged = registry.findByMac(CENTRAL_MAC);
    reportCheck("A no-op registerSelf() leaves the existing friendlyName untouched",
                unchanged != nullptr && std::strcmp(unchanged->friendlyName, "Central") == 0);
}

void testFirstCommissioningRoundTrip() {
    printSection("TEST 3 - FIRST COMMISSIONING ROUND TRIP");

    NodeCommissioningRegistry registry;
    reportCheck("requestCommissioning() before discovery is rejected (no fake Node from a command alone)",
                !registry.requestCommissioning(SMART_MAC, "Sitting Room"));

    registry.recordDiscovered(SMART_MAC, NodeRole::SMART, "0.2.0-foundation", "esp32:2core", 1000U);

    reportCheck("requestCommissioning() with an empty name is rejected",
                !registry.requestCommissioning(SMART_MAC, ""));
    reportCheck("requestCommissioning() with a name exceeding the buffer is rejected",
                !registry.requestCommissioning(SMART_MAC, "This friendly name is far too long to fit"));

    reportCheck("requestCommissioning() with a valid name is accepted", registry.requestCommissioning(SMART_MAC, "Sitting Room"));

    const NodeCommissioningRegistry::CommissioningRecord* inFlight = registry.findByMac(SMART_MAC);
    reportCheck("Accepted first commissioning moves lifecycleState to CONFIGURING",
                inFlight != nullptr && inFlight->lifecycleState == NodeLifecycleState::CONFIGURING);
    reportCheck("Accepted commissioning sets syncState to PENDING",
                inFlight != nullptr && inFlight->syncState == NodeCommissioningRegistry::SyncState::PENDING);
    reportCheck("friendlyName is NOT committed yet while PENDING",
                inFlight != nullptr && inFlight->friendlyName[0] == '\0');

    reportCheck("A second requestCommissioning() while already CONFIGURING is rejected",
                !registry.requestCommissioning(SMART_MAC, "Another Name"));

    reportCheck("applyCommissionResult() for an unknown MAC returns false",
                !registry.applyCommissionResult(OTHER_MAC, true, NodeLifecycleState::COMMISSIONED));

    reportCheck("applyCommissionResult() with success=true, resultingState=COMMISSIONED is accepted",
                registry.applyCommissionResult(SMART_MAC, true, NodeLifecycleState::COMMISSIONED));

    const NodeCommissioningRegistry::CommissioningRecord* commissioned = registry.findByMac(SMART_MAC);
    reportCheck("lifecycleState becomes COMMISSIONED", commissioned != nullptr &&
                commissioned->lifecycleState == NodeLifecycleState::COMMISSIONED);
    reportCheck("friendlyName is now committed", commissioned != nullptr &&
                std::strcmp(commissioned->friendlyName, "Sitting Room") == 0);
    reportCheck("syncState becomes SYNCED", commissioned != nullptr &&
                commissioned->syncState == NodeCommissioningRegistry::SyncState::SYNCED);
}

void testFailedCommissioningRollsBack() {
    printSection("TEST 4 - FAILED COMMISSIONING NEVER PARTIALLY APPLIES");

    NodeCommissioningRegistry registry;
    registry.recordDiscovered(SMART_MAC, NodeRole::SMART, "0.2.0-foundation", "esp32:2core", 1000U);
    registry.requestCommissioning(SMART_MAC, "Sitting Room");

    reportCheck("applyCommissionResult() with success=false, resultingState=UNCOMMISSIONED is accepted",
                registry.applyCommissionResult(SMART_MAC, false, NodeLifecycleState::UNCOMMISSIONED));

    const NodeCommissioningRegistry::CommissioningRecord* rolledBack = registry.findByMac(SMART_MAC);
    reportCheck("A failed commission rolls lifecycleState back to what the Node itself reported",
                rolledBack != nullptr && rolledBack->lifecycleState == NodeLifecycleState::UNCOMMISSIONED);
    reportCheck("A failed commission never commits the pending friendlyName",
                rolledBack != nullptr && rolledBack->friendlyName[0] == '\0');
    reportCheck("A failed commission sets syncState to FAILED",
                rolledBack != nullptr && rolledBack->syncState == NodeCommissioningRegistry::SyncState::FAILED);
}

void testRename() {
    printSection("TEST 5 - RENAME AN ALREADY-COMMISSIONED NODE");

    NodeCommissioningRegistry registry;
    registry.recordDiscovered(SMART_MAC, NodeRole::SMART, "0.2.0-foundation", "esp32:2core", 1000U);
    registry.requestCommissioning(SMART_MAC, "Sitting Room");
    registry.applyCommissionResult(SMART_MAC, true, NodeLifecycleState::COMMISSIONED);

    reportCheck("requestCommissioning() (rename) on an already-COMMISSIONED Node is accepted",
                registry.requestCommissioning(SMART_MAC, "Living Room"));

    const NodeCommissioningRegistry::CommissioningRecord* renaming = registry.findByMac(SMART_MAC);
    reportCheck("A rename does not disturb lifecycleState (stays COMMISSIONED, never CONFIGURING)",
                renaming != nullptr && renaming->lifecycleState == NodeLifecycleState::COMMISSIONED);
    reportCheck("The old friendlyName is untouched until the rename is acknowledged",
                renaming != nullptr && std::strcmp(renaming->friendlyName, "Sitting Room") == 0);

    registry.applyCommissionResult(SMART_MAC, true, NodeLifecycleState::COMMISSIONED);
    const NodeCommissioningRegistry::CommissioningRecord* renamed = registry.findByMac(SMART_MAC);
    reportCheck("After a successful rename ACK, friendlyName is updated",
                renamed != nullptr && std::strcmp(renamed->friendlyName, "Living Room") == 0);
}

void testDecommissionAndRediscovery() {
    printSection("TEST 6 - DECOMMISSION AND REDISCOVERY");

    NodeCommissioningRegistry registry;
    reportCheck("decommission() on an unknown MAC returns false", !registry.decommission(SMART_MAC));

    registry.recordDiscovered(SMART_MAC, NodeRole::SMART, "0.2.0-foundation", "esp32:2core", 1000U);
    registry.requestCommissioning(SMART_MAC, "Sitting Room");
    registry.applyCommissionResult(SMART_MAC, true, NodeLifecycleState::COMMISSIONED);

    reportCheck("decommission() on a COMMISSIONED Node is accepted", registry.decommission(SMART_MAC));

    const NodeCommissioningRegistry::CommissioningRecord* decommissioned = registry.findByMac(SMART_MAC);
    reportCheck("lifecycleState becomes DECOMMISSIONED",
                decommissioned != nullptr && decommissioned->lifecycleState == NodeLifecycleState::DECOMMISSIONED);
    reportCheck("friendlyName is cleared on decommission",
                decommissioned != nullptr && decommissioned->friendlyName[0] == '\0');

    reportCheck("Rediscovering a DECOMMISSIONED MAC moves it back to UNCOMMISSIONED",
                !registry.recordDiscovered(SMART_MAC, NodeRole::SMART, "0.2.1", "esp32:2core", 5000U));
    const NodeCommissioningRegistry::CommissioningRecord* rediscovered = registry.findByMac(SMART_MAC);
    reportCheck("The rediscovered record is UNCOMMISSIONED again, eligible for re-commissioning",
                rediscovered != nullptr && rediscovered->lifecycleState == NodeLifecycleState::UNCOMMISSIONED);
}

void testPersistenceHonestOnHostBuild() {
    printSection("TEST 7 - PERSISTENCE IS HONEST ON A HOST BUILD");

    NodeCommissioningRegistry registry;
    registry.registerSelf(CENTRAL_MAC, NodeRole::CENTRAL, "Central", "0.2.0-foundation", "esp32:2core",
                           nullptr, 0U, 100U);

    reportCheck("persist() on a host build reports failure, not a fabricated success", !registry.persist());

    NodeCommissioningRegistry freshRegistry;
    reportCheck("loadPersisted() on a host build reports failure, not a fabricated restore",
                !freshRegistry.loadPersisted());
    reportCheck("A failed loadPersisted() leaves the registry with zero records",
                freshRegistry.getCount() == 0U);
}

} // namespace

int main() {
    std::printf("KILOWATTS NODECOMMISSIONINGREGISTRY HOST TEST REPORT\n");

    testDiscovery();
    testRelayCapabilities();
    testRegisterSelf();
    testFirstCommissioningRoundTrip();
    testFailedCommissioningRollsBack();
    testRename();
    testDecommissionAndRediscovery();
    testPersistenceHonestOnHostBuild();

    printSection("FINAL TEST SUMMARY");
    std::printf("Passed checks: %zu\n", passedChecks);
    std::printf("Failed checks: %zu\n", failedChecks);
    std::printf("OVERALL RESULT: %s\n", failedChecks == 0U ? "PASS" : "FAIL");

    return failedChecks == 0U ? 0 : 1;
}
