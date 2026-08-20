/**
 * @file test_system_state_json.cpp
 * @brief Host-native correctness tests for the kilowatts/v1/state/system
 *        JSON payload builder.
 *
 * This file uses a standard host int main(), not an ESP-IDF app_main(), so
 * it can be compiled and run by run_cpp_test.sh's plain g++ invocation.
 */

#include "SystemStateJson.h"

#include <cstdio>
#include <cstring>

using kilowatts::SystemStateInputs;
using kilowatts::SystemStateJson;

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

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

SystemStateInputs makeInputs() {
    SystemStateInputs inputs{};
    inputs.batterySensorConfigured = true;
    inputs.batteryVoltageVolts = 12.6F;
    inputs.batteryCurrentAmps = 8.0F;
    inputs.batteryMeasurementSourceText = "HARDWARE";
    inputs.stateOfChargePercent = 76.5F;
    inputs.stateOfChargeValid = true;
    inputs.stateOfChargeSourceText = "COULOMB_COUNTING";
    inputs.estimatedTotalLoadPowerWatts = 40.0F;
    inputs.availablePowerWatts = 162.0F;
    inputs.fixedOnRunningPowerWatts = 12.0F;
    inputs.powerAvailableForAutoLoadsWatts = 150.0F;
    inputs.remainingPowerWatts = 97.0F;
    inputs.committedPowerWatts = 65.0F;
    inputs.wifiConnected = true;
    inputs.wifiStateText = "CONNECTED_WITH_IP";
    inputs.mqttConnected = true;
    inputs.currentTimeValid = true;
    inputs.currentTimeSourceText = "NTP";
    inputs.lastOptimizationEpochSeconds = 1765000000;
    inputs.faultCount = 0U;
    inputs.faultSummaryText = "";
    return inputs;
}

void testValidJsonStructure() {
    printSection("TEST 1 - VALID JSON STRUCTURE");

    const std::string json = SystemStateJson::build(makeInputs(), 1U);

    int braceBalance = 0;
    for (char c : json) {
        if (c == '{') ++braceBalance;
        if (c == '}') --braceBalance;
    }

    reportCheck("Every '{' has a matching '}'", braceBalance == 0);
    reportCheck("JSON starts with '{'", !json.empty() && json.front() == '{');
    reportCheck("JSON ends with '}'", !json.empty() && json.back() == '}');
    reportCheck("No trailing comma directly before a closing brace", json.find(",}") == std::string::npos);
}

void testEveryRequiredFieldIsPresent() {
    printSection("TEST 2 - EVERY REQUIRED FIELD IS PRESENT");

    const std::string json = SystemStateJson::build(makeInputs(), 1U);

    reportCheck("schemaVersion is present", contains(json, "\"schemaVersion\":1"));
    reportCheck("Battery voltage is present", contains(json, "\"voltageVolts\":12.600"));
    reportCheck("Battery current is present", contains(json, "\"currentAmps\":8.000"));
    reportCheck("State of charge is present", contains(json, "\"stateOfChargePercent\":76.500"));
    reportCheck("Estimated rated load power is present", contains(json, "\"estimatedTotalLoadPowerWatts\":40.000"));
    reportCheck("Available power (P_available) is present", contains(json, "\"availablePowerWatts\":162.000"));
    reportCheck("Fixed ON running power is present", contains(json, "\"fixedOnRunningPowerWatts\":12.000"));
    reportCheck("Power available for Auto Loads is present",
                contains(json, "\"powerAvailableForAutoLoadsWatts\":150.000"));
    reportCheck("P_remaining is present", contains(json, "\"remainingPowerWatts\":97.000"));
    reportCheck("P_committed is present", contains(json, "\"committedPowerWatts\":65.000"));
    reportCheck("Wi-Fi connectivity is present", contains(json, "\"wifiConnected\":true"));
    reportCheck("MQTT connectivity is present", contains(json, "\"mqttConnected\":true"));
    reportCheck("Current-time validity is present", contains(json, "\"valid\":true"));
    reportCheck("Current-time source is present", contains(json, "\"source\":\"NTP\""));
    reportCheck("Last optimization timestamp is present",
                contains(json, "\"lastOptimizationEpochSeconds\":1765000000"));
    reportCheck("Battery sensor configured flag is present", contains(json, "\"sensorConfigured\":true"));
    reportCheck("Battery measurement source is present", contains(json, "\"measurementSource\":\"HARDWARE\""));
    reportCheck("SoC validity is present", contains(json, "\"stateOfChargeValid\":true"));
    reportCheck("SoC source is present", contains(json, "\"stateOfChargeSource\":\"COULOMB_COUNTING\""));
    reportCheck("Fault count is present", contains(json, "\"faultCount\":0"));
}

void testSensorSourceLabelling() {
    printSection("TEST 3 - BATTERY SOURCE LABELLING");

    SystemStateInputs hardwareInputs = makeInputs();
    hardwareInputs.batteryMeasurementSourceText = "HARDWARE";
    const std::string hardwareJson = SystemStateJson::build(hardwareInputs, 1U);
    reportCheck("Hardware battery source is explicitly labelled",
                contains(hardwareJson, "\"measurementSource\":\"HARDWARE\""));

    SystemStateInputs unconfiguredInputs = makeInputs();
    unconfiguredInputs.batterySensorConfigured = false;
    unconfiguredInputs.batteryMeasurementSourceText = "NONE";
    unconfiguredInputs.stateOfChargeValid = false;
    unconfiguredInputs.stateOfChargeSourceText = "UNKNOWN";
    const std::string unconfiguredJson = SystemStateJson::build(unconfiguredInputs, 1U);
    reportCheck("An unconfigured battery sensor is explicitly labelled, not silently defaulted",
                contains(unconfiguredJson, "\"sensorConfigured\":false") &&
                contains(unconfiguredJson, "\"measurementSource\":\"NONE\""));
    reportCheck("Invalid/unknown SoC is explicitly labelled, never presented as a genuine reading",
                contains(unconfiguredJson, "\"stateOfChargeValid\":false") &&
                contains(unconfiguredJson, "\"stateOfChargeSource\":\"UNKNOWN\""));
}

void testFaultSummaryEscaping() {
    printSection("TEST 4 - FAULT SUMMARY STRING ESCAPING");

    SystemStateInputs faulted = makeInputs();
    faulted.faultCount = 2U;
    faulted.faultSummaryText = "Relay \"pin16\" failed\nconfirmation";

    const std::string json = SystemStateJson::build(faulted, 1U);

    reportCheck("faultCount reflects the supplied value", contains(json, "\"faultCount\":2"));
    reportCheck("Embedded quote in faultSummary is escaped", contains(json, "\\\"pin16\\\""));
    reportCheck("Embedded newline in faultSummary is escaped", contains(json, "\\n"));
    reportCheck("No raw (unescaped) newline landed in the JSON output", json.find('\n') == std::string::npos);
}

} // namespace

int main() {
    std::printf("KILOWATTS SYSTEMSTATEJSON HOST TEST REPORT\n");

    testValidJsonStructure();
    testEveryRequiredFieldIsPresent();
    testSensorSourceLabelling();
    testFaultSummaryEscaping();

    printSection("FINAL TEST SUMMARY");
    std::printf("Passed checks: %zu\n", passedChecks);
    std::printf("Failed checks: %zu\n", failedChecks);
    std::printf("OVERALL RESULT: %s\n", failedChecks == 0U ? "PASS" : "FAIL");

    return failedChecks == 0U ? 0 : 1;
}
