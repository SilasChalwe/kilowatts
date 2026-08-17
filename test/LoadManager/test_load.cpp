/**
 * @file test_load.cpp
 * @brief Host-native correctness tests for Load's state-authority
 *        separation (configured mode vs planning target vs confirmed
 *        relay state/validity vs health).
 *
 * These tests exist for the correction pass described in this project's
 * "STATE AUTHORITY, ACTUATION ORDER, SAFETY RE-CHECKS, FIXED-LOAD
 * PROTECTION, NODE/SENSOR FAILURE HANDLING" specification: Load must keep
 * getMode(), getTargetRelayState(), and
 * getConfirmedRelayState()/isConfirmedRelayStateValid() as three
 * genuinely independent pieces of state that never overwrite one
 * another, and a caller that only calls setHealth() on a failed relay
 * command (never setConfirmedRelayState()) must leave the previous
 * trusted confirmed state and its validity completely untouched — this
 * is the exact pattern src/central/main.cpp and src/smart/main.cpp are
 * both required to follow (see RelayCommandDispatcher's own README/tests
 * for the matching dispatch-order-level guarantee).
 *
 * This file uses a standard host int main(), not an ESP-IDF app_main(),
 * so it can be compiled and run by run_cpp_test.sh's plain g++
 * invocation — matching test/LoadFilter/test_load_filter.cpp.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 14 August 2026
 */

#include "Load.h"

#include <cstdio>

using kilowatts::Load;
using kilowatts::LoadElectricalRatings;
using kilowatts::LoadHealth;
using kilowatts::LoadMode;
using kilowatts::LoadPower;

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
 * TEST 1 - CONFIRMED RELAY STATE STARTS INVALID
 */
void testConfirmedRelayStateStartsInvalid() {
    printSection("TEST 1 - CONFIRMED RELAY STATE STARTS INVALID");

    Load light(Load::Id{DEMO_MAC, 16U}, "Light", LoadPower{6.0F, 6.0F}, 8U, LoadMode::Fixed::ON);

    reportCheck("A freshly constructed Load has no trustworthy confirmed relay state yet",
                !light.isConfirmedRelayStateValid());
    reportCheck("A freshly constructed Load defaults to healthy (AVAILABLE)",
                light.getHealth() == LoadHealth::AVAILABLE);
}


/**
 * TEST 2 - SUCCESSFUL CONFIRMATION MARKS VALID
 */
void testSuccessfulConfirmationMarksValid() {
    printSection("TEST 2 - SUCCESSFUL CONFIRMATION MARKS VALID");

    Load fan(Load::Id{DEMO_MAC, 17U}, "Fan", LoadPower{35.0F, 55.0F}, 5U, LoadMode::Auto::OFF);

    fan.setConfirmedRelayState(true);

    reportCheck("setConfirmedRelayState(true) is reflected by getConfirmedRelayState()",
                fan.getConfirmedRelayState());
    reportCheck("setConfirmedRelayState() always marks the confirmation valid",
                fan.isConfirmedRelayStateValid());
}


/**
 * TEST 3 - A FAILED RELAY COMMAND MUST NOT CHANGE THE CONFIRMED STATE
 *
 * This is the exact failure pattern a caller (RelayCommandDispatcher's
 * consumer in src/central/main.cpp / src/smart/main.cpp) must follow: on
 * a failed/timed-out relay command, only Load::setHealth() is called —
 * Load::setConfirmedRelayState() is never called. A failed
 * acknowledgement does not prove the relay is OFF, so the previously
 * trusted confirmed state (and its validity) must survive completely
 * unchanged, exactly as if the failed command had never been attempted.
 */
void testFailedCommandPreservesPreviousConfirmedState() {
    printSection("TEST 3 - FAILED RELAY COMMAND PRESERVES PREVIOUS CONFIRMED STATE");

    Load pump(Load::Id{DEMO_MAC, 19U}, "WaterPump", LoadPower{35.0F, 55.0F}, 4U, LoadMode::Auto::ON);

    // Start ON and confirmed, as if an earlier successful command already ran.
    pump.setConfirmedRelayState(true);
    reportCheck("Setup: WaterPump starts confirmed ON", pump.getConfirmedRelayState());
    reportCheck("Setup: WaterPump starts confirmation-valid", pump.isConfirmedRelayStateValid());

    // Simulate a failed OFF command: the correct caller pattern only calls
    // setHealth(FAULTED) here, never setConfirmedRelayState().
    pump.setHealth(LoadHealth::FAULTED);

    reportCheck("A failed command does not flip the confirmed relay state to OFF",
                pump.getConfirmedRelayState());
    reportCheck("A failed command does not invalidate the previous trusted confirmation",
                pump.isConfirmedRelayStateValid());
    reportCheck("A failed command does mark the Load FAULTED",
                pump.getHealth() == LoadHealth::FAULTED);

    // A later successful command is the only thing that may change the confirmed state.
    pump.setConfirmedRelayState(false);
    reportCheck("A subsequent successful confirmation may still change the confirmed state",
                !pump.getConfirmedRelayState());
    reportCheck("...and remains valid", pump.isConfirmedRelayStateValid());
}


/**
 * TEST 4 - CONFIGURED MODE, TARGET STATE AND CONFIRMED STATE ARE INDEPENDENT
 *
 * Mirrors BestFirstSearch's own
 * testConfiguredModeNeverMutatedByBestFirstDecision() from the Load side:
 * setTargetRelayState() must never be observable through getMode()/isOn(),
 * and vice versa.
 */
void testTargetStateIndependentOfConfiguredMode() {
    printSection("TEST 4 - TARGET STATE INDEPENDENT OF CONFIGURED MODE");

    Load pump(Load::Id{DEMO_MAC, 19U}, "WaterPump", LoadPower{35.0F, 55.0F}, 4U, LoadMode::Auto::OFF);

    reportCheck("A newly constructed AUTO_OFF Load's initial target mirrors its configured state (OFF)",
                !pump.getTargetRelayState());

    // Best-First Search admits this AUTO_OFF Load this cycle: only the target changes.
    pump.setTargetRelayState(true);

    reportCheck("setTargetRelayState(true) is reflected by getTargetRelayState()",
                pump.getTargetRelayState());
    reportCheck("...but getMode() still reports the configured AUTO_OFF value",
                pump.getMode() == LoadMode::Auto::OFF);
    reportCheck("...and isOn() (the configured value) still reports OFF",
                !pump.isOn());

    // Confirmed state is a third, still-independent axis.
    pump.setConfirmedRelayState(false);
    reportCheck("Target ON but not yet confirmed: confirmed state remains OFF until hardware proves otherwise",
                !pump.getConfirmedRelayState());
}

/**
 * TEST 5 - AN INVALID WIRE CONFIRMATION MUST NOT LAUNDER "UNKNOWN" INTO "OFF"
 *
 * Mirrors the exact caller contract CentralNodeRegistry::applyNodeReport()
 * now follows for a received NodeReportPacket's
 * LoadReportPacket::confirmedRelayStateValid byte: setConfirmedRelayState()
 * must only be called when the wire report itself vouches for the value
 * (confirmedRelayStateValid != 0). Since CentralNodeRegistry is
 * ESP32-target-only (it depends on EspNowCommunication/NodeReportPackets,
 * which require real ESP-IDF headers) and has no host test of its own,
 * this test pins the same contract at the Load level that the registry's
 * one-line guard relies on: a caller that correctly withholds the
 * setConfirmedRelayState() call must leave the Load's previous confirmed
 * state and validity completely untouched, and a caller that does call it
 * (because the wire report was valid) must still update normally.
 */
void testWireConfirmationValidityGatesWhetherConfirmedStateIsApplied() {
    printSection("TEST 5 - WIRE CONFIRMATION VALIDITY GATES CONFIRMED STATE UPDATES");

    Load fan(Load::Id{DEMO_MAC, 17U}, "Fan", LoadPower{35.0F, 55.0F}, 5U, LoadMode::Auto::ON);

    // An earlier report already established a trusted confirmed ON state.
    fan.setConfirmedRelayState(true);
    reportCheck("Setup: Fan starts confirmed ON and valid",
                fan.getConfirmedRelayState() && fan.isConfirmedRelayStateValid());

    // Simulate applyNodeReport() receiving a report whose
    // confirmedRelayStateValid wire byte is 0: the correct caller pattern
    // never calls setConfirmedRelayState() in this branch at all.
    const std::uint8_t wireConfirmedRelayStateValid = 0U;
    if (wireConfirmedRelayStateValid != 0U) {
        fan.setConfirmedRelayState(false);
    }

    reportCheck("An invalid wire report does not flip the confirmed relay state to OFF",
                fan.getConfirmedRelayState());
    reportCheck("An invalid wire report does not invalidate the previous trusted confirmation",
                fan.isConfirmedRelayStateValid());

    // A later report whose confirmedRelayStateValid wire byte is 1 must
    // still update the Load normally.
    const std::uint8_t nextWireConfirmedRelayStateValid = 1U;
    const std::uint8_t nextWireConfirmedRelayState = 0U;
    if (nextWireConfirmedRelayStateValid != 0U) {
        fan.setConfirmedRelayState(nextWireConfirmedRelayState != 0U);
    }

    reportCheck("A valid wire report does update the confirmed relay state",
                !fan.getConfirmedRelayState());
    reportCheck("...and remains valid", fan.isConfirmedRelayStateValid());
}

/**
 * TEST 6 - INSTALLER RATINGS ARE DISTINCT FROM LIVE MEASUREMENTS
 *
 * The one-sensor design plans each Smart-node load from the installer
 * voltage/current pair. That pair must accept only a complete, finite,
 * positive rating (or the explicit legacy zero/zero absence), and must not
 * be confused with the generic LoadMeasurements API.
 */
void testInstallerElectricalRatings() {
    printSection("TEST 6 - INSTALLER ELECTRICAL RATINGS");

    Load pump(Load::Id{DEMO_MAC, 21U}, "Pump", LoadPower{72.0F, 120.0F},
              6U, LoadMode::Auto::OFF);

    reportCheck("A complete nominal voltage/current pair is accepted",
                pump.setElectricalRatings(LoadElectricalRatings{24.0F, 3.0F}));
    const LoadElectricalRatings ratings = pump.getElectricalRatings();
    reportCheck("The accepted installer ratings are retained exactly",
                ratings.nominalVoltageVolts == 24.0F && ratings.nominalCurrentAmps == 3.0F);
    reportCheck("A partial voltage/current rating is rejected",
                !pump.setElectricalRatings(LoadElectricalRatings{24.0F, 0.0F}));
    reportCheck("A rejected partial rating leaves the previous complete rating unchanged",
                pump.getElectricalRatings().nominalVoltageVolts == 24.0F &&
                pump.getElectricalRatings().nominalCurrentAmps == 3.0F);
}

} // namespace


int main() {
    testConfirmedRelayStateStartsInvalid();
    testSuccessfulConfirmationMarksValid();
    testFailedCommandPreservesPreviousConfirmedState();
    testTargetStateIndependentOfConfiguredMode();
    testWireConfirmationValidityGatesWhetherConfirmedStateIsApplied();
    testInstallerElectricalRatings();

    std::printf("\n======================================================================\n");
    std::printf("RESULTS: %zu passed, %zu failed\n", passedChecks, failedChecks);
    std::printf("======================================================================\n");

    return failedChecks == 0U ? 0 : 1;
}
