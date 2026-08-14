/**
 * @file test_relay_controller.cpp
 * @brief Host-native correctness tests for RelayController's
 *        hardware-free registration and commanded-state bookkeeping.
 *
 * The real GPIO configuration/read/write calls are compiled only under
 * ESP_PLATFORM (see RelayController.cpp), so this host build exercises
 * everything that must be true regardless of hardware: duplicate-pin
 * rejection, commanded-state tracking, and that a host build never
 * fabricates a hardware-applied or read-back result.
 *
 * This file uses a standard host int main(), not an ESP-IDF app_main(), so
 * it can be compiled and run by run_cpp_test.sh's plain g++ invocation.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
 */

#include "RelayController.h"

#include <cstdio>

using kilowatts::RelayController;

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

void testRegistration() {
    printSection("TEST 1 - RELAY REGISTRATION");

    RelayController relays;

    reportCheck("No relays registered initially", relays.getNumberOfRelays() == 0U);
    reportCheck("isRelayRegistered() is false before addRelay()", !relays.isRelayRegistered(16U));

    reportCheck("addRelay() accepts pin 16",
                relays.addRelay(RelayController::RelayConfiguration{16U, false, false}));
    reportCheck("addRelay() accepts pin 17",
                relays.addRelay(RelayController::RelayConfiguration{17U, true, false}));

    reportCheck("Two relays are registered", relays.getNumberOfRelays() == 2U);
    reportCheck("isRelayRegistered() is true after addRelay()", relays.isRelayRegistered(16U));

    reportCheck("addRelay() rejects a duplicate pin",
                !relays.addRelay(RelayController::RelayConfiguration{16U, false, true}));
    reportCheck("Duplicate rejection did not add a third relay", relays.getNumberOfRelays() == 2U);

    const RelayController::RelayConfiguration* first = relays.getRelay(0U);
    reportCheck("getRelay(0) returns pin 16's configuration",
                first != nullptr && first->relayPin == 16U && !first->activeHigh);

    reportCheck("getRelay() on an out-of-range index returns nullptr", relays.getRelay(99U) == nullptr);
}

void testCommandedStateTracking() {
    printSection("TEST 2 - COMMANDED STATE TRACKING (HOST BUILD, NO REAL GPIO)");

    RelayController relays;
    relays.addRelay(RelayController::RelayConfiguration{16U, false, /* initialStateOn */ false});

    bool commanded = true;
    reportCheck("Initial commanded state matches initialStateOn=false",
                relays.getCommandedState(16U, commanded) && !commanded);

    /*
     * On a host build there is no real GPIO, so setRelayState()'s
     * underlying write always fails and the call itself returns false —
     * but the commanded state must still be recorded, exactly as
     * RelayController.cpp documents, so a later readback comparison can
     * detect the mismatch as a real fault rather than losing the command.
     */
    const bool writeResult = relays.setRelayState(16U, true);
    reportCheck("setRelayState() reports failure on a host build (no real GPIO)", !writeResult);

    commanded = false;
    reportCheck("Commanded state was still recorded as ON despite the reported hardware failure",
                relays.getCommandedState(16U, commanded) && commanded);

    reportCheck("setRelayState() on an unregistered pin is rejected", !relays.setRelayState(99U, true));

    bool unusedState = false;
    reportCheck("getCommandedState() on an unregistered pin returns false", !relays.getCommandedState(99U, unusedState));
}

void testHardwareApplicationNeverFabricatedOnHost() {
    printSection("TEST 3 - HOST BUILD NEVER FABRICATES HARDWARE STATE");

    RelayController relays;
    relays.addRelay(RelayController::RelayConfiguration{16U, false, false});

    reportCheck("isHardwareApplied() is false on a host build", !relays.isHardwareApplied(16U));

    bool readBack = false;
    reportCheck("readBackState() returns false on a host build (no real GPIO to read)",
                !relays.readBackState(16U, readBack));

    reportCheck("isHardwareApplied() on an unregistered pin returns false", !relays.isHardwareApplied(99U));
}

} // namespace

int main() {
    std::printf("KILOWATTS RELAYCONTROLLER HOST TEST REPORT\n");
    std::printf("Author: Chalwe Silas\n");
    std::printf("Programme: Final-Year Computer Engineering\n");
    std::printf("Institution: The Copperbelt University\n");

    testRegistration();
    testCommandedStateTracking();
    testHardwareApplicationNeverFabricatedOnHost();

    printSection("FINAL TEST SUMMARY");
    std::printf("Passed checks: %zu\n", passedChecks);
    std::printf("Failed checks: %zu\n", failedChecks);
    std::printf("OVERALL RESULT: %s\n", failedChecks == 0U ? "PASS" : "FAIL");

    return failedChecks == 0U ? 0 : 1;
}
