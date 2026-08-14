/**
 * @file test_development_session.cpp
 * @brief Host-native correctness tests for DevelopmentSession's
 *        environment/override bookkeeping.
 *
 * This file uses a standard host int main(), not an ESP-IDF app_main(), so
 * it can be compiled and run by run_cpp_test.sh's plain g++ invocation.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 14 August 2026
 */

#include "DevelopmentSession.h"

#include <cstdio>
#include <cstring>

using kilowatts::DevelopmentSession;
using kilowatts::OperatingEnvironment;
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

/**
 * TEST 1 - PRODUCTION IS ALWAYS THE DEFAULT
 */
void testProductionIsDefault() {
    printSection("TEST 1 - PRODUCTION IS ALWAYS THE DEFAULT");

    DevelopmentSession session;
    reportCheck("A new session starts PRODUCTION", session.getEnvironment() == OperatingEnvironment::PRODUCTION);
    reportCheck("A new session is not active", !session.isActive());
    reportCheck("A new session has zero sensor overrides", session.getNumberOfSensorOverrides() == 0U);
}

/**
 * TEST 2 - START / END
 */
void testStartEnd() {
    printSection("TEST 2 - START / END");

    DevelopmentSession session;

    reportCheck("start() from PRODUCTION is accepted", session.start());
    reportCheck("Environment becomes DEVELOPMENT", session.getEnvironment() == OperatingEnvironment::DEVELOPMENT);
    reportCheck("isActive() becomes true", session.isActive());

    reportCheck("A second start() while already active is rejected", !session.start());
    reportCheck("A rejected start() leaves the environment unchanged", session.isActive());

    reportCheck("end() from DEVELOPMENT is accepted", session.end());
    reportCheck("Environment returns to PRODUCTION", session.getEnvironment() == OperatingEnvironment::PRODUCTION);

    reportCheck("A second end() while already inactive is rejected", !session.end());
}

/**
 * TEST 3 - SENSOR OVERRIDES REQUIRE AN ACTIVE SESSION
 */
void testOverridesRequireActiveSession() {
    printSection("TEST 3 - SENSOR OVERRIDES REQUIRE AN ACTIVE SESSION");

    DevelopmentSession session;

    reportCheck("setSensorOverride() while inactive is rejected", !session.setSensorOverride(0x40U, 12.0F, 2.0F));

    float v = 0.0F, i = 0.0F;
    reportCheck("findSensorOverride() while inactive returns false", !session.findSensorOverride(0x40U, v, i));
    reportCheck("clearSensorOverride() while inactive is rejected", !session.clearSensorOverride(0x40U));

    session.start();
    reportCheck("setSensorOverride() while active is accepted", session.setSensorOverride(0x40U, 12.0F, 2.0F));
    reportCheck("getNumberOfSensorOverrides() becomes 1", session.getNumberOfSensorOverrides() == 1U);

    reportCheck("findSensorOverride() while active finds the armed override",
                session.findSensorOverride(0x40U, v, i) && v == 12.0F && i == 2.0F);
    reportCheck("findSensorOverride() for a different address finds nothing",
                !session.findSensorOverride(0x41U, v, i));

    reportCheck("setSensorOverride() again for the same address replaces it, not duplicates",
                session.setSensorOverride(0x40U, 13.5F, 4.2F) && session.getNumberOfSensorOverrides() == 1U);
    reportCheck("The replaced override reads back the new values",
                session.findSensorOverride(0x40U, v, i) && v == 13.5F && i == 4.2F);

    reportCheck("clearSensorOverride() while active removes it", session.clearSensorOverride(0x40U));
    reportCheck("getNumberOfSensorOverrides() returns to 0", session.getNumberOfSensorOverrides() == 0U);
    reportCheck("clearSensorOverride() on an already-cleared address is rejected", !session.clearSensorOverride(0x40U));
}

/**
 * TEST 4 - END DISCARDS EVERY OVERRIDE
 */
void testEndDiscardsOverrides() {
    printSection("TEST 4 - END DISCARDS EVERY OVERRIDE");

    DevelopmentSession session;
    session.start();
    session.setSensorOverride(0x40U, 12.0F, 2.0F);
    session.setSensorOverride(0x41U, 5.0F, 0.5F);
    reportCheck("Two overrides are armed before end()", session.getNumberOfSensorOverrides() == 2U);

    session.end();
    reportCheck("end() discards every override", session.getNumberOfSensorOverrides() == 0U);

    session.start();
    reportCheck("A fresh session after end() has zero overrides carried over", session.getNumberOfSensorOverrides() == 0U);
    float v = 0.0F, i = 0.0F;
    reportCheck("A fresh session cannot find an override from a previous, already-ended session",
                !session.findSensorOverride(0x40U, v, i));
}

/**
 * TEST 5 - TEXT REPRESENTATION
 */
void testTextRepresentation() {
    printSection("TEST 5 - TEXT REPRESENTATION");

    reportCheck("toText(PRODUCTION) == \"PRODUCTION\"",
                std::strcmp(toText(OperatingEnvironment::PRODUCTION), "PRODUCTION") == 0);
    reportCheck("toText(DEVELOPMENT) == \"DEVELOPMENT\"",
                std::strcmp(toText(OperatingEnvironment::DEVELOPMENT), "DEVELOPMENT") == 0);
}

} // namespace

int main() {
    std::printf("KILOWATTS DEVELOPMENTSESSION HOST TEST REPORT\n");
    std::printf("Author: Chalwe Silas\n");
    std::printf("Programme: Final-Year Computer Engineering\n");
    std::printf("Institution: The Copperbelt University\n");

    testProductionIsDefault();
    testStartEnd();
    testOverridesRequireActiveSession();
    testEndDiscardsOverrides();
    testTextRepresentation();

    printSection("FINAL TEST SUMMARY");
    std::printf("Passed checks: %zu\n", passedChecks);
    std::printf("Failed checks: %zu\n", failedChecks);
    std::printf("OVERALL RESULT: %s\n", failedChecks == 0U ? "PASS" : "FAIL");

    return failedChecks == 0U ? 0 : 1;
}
