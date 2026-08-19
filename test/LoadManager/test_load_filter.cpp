/**
 * @file test_load_filter.cpp
 * @brief Host-native correctness tests for LoadFilter's classification-only
 *        responsibility.
 *
 * LoadFilter's Fixed ON Running Power accounting has moved to
 * AvailablePowerManager (see test/AvailablePowerManager/) — this file
 * verifies only the classification behaviour LoadFilter itself still owns:
 * FIXED_ON/FIXED_OFF/AUTO_ON/AUTO_OFF routing, pointer identity to the
 * original Load objects, and that no power total is calculated or stored
 * here any more.
 *
 * This file uses a standard host int main(), not an ESP-IDF app_main(), so
 * it can be compiled and run by run_cpp_test.sh's plain g++ invocation —
 * matching test/BestFirstSearch/test_best_first_search.cpp.
 */

#include "Load.h"
#include "LoadFilter.h"

#include <cstdio>

using kilowatts::Load;
using kilowatts::LoadFilter;
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

void testFixedOnClassification() {
    printSection("TEST 1 - FIXED ON CLASSIFICATION");

    Load light(Load::Id{DEMO_MAC, 16U}, "Light", LoadPower{12.0F, 12.0F}, 1U, LoadMode::Fixed::ON);

    LoadFilter loadFilter;
    const bool added = loadFilter.addLoad(light);

    reportCheck("Fixed::ON Load is accepted by addLoad()", added);
    reportCheck("Fixed::ON Load is counted in the Fixed ON collection",
                loadFilter.getNumberOfFixedOnLoads() == 1U);
    reportCheck("Fixed::ON Load does not appear in Fixed OFF",
                loadFilter.getNumberOfFixedOffLoads() == 0U);
    reportCheck("Fixed::ON Load does not appear in Auto candidates",
                loadFilter.getNumberOfAutoCandidateLoads() == 0U);
    reportCheck("getFixedOnLoad(0) returns the same Load object (pointer identity)",
                loadFilter.getFixedOnLoad(0U) == &light);
}

void testFixedOffClassification() {
    printSection("TEST 2 - FIXED OFF CLASSIFICATION");

    Load pump(Load::Id{DEMO_MAC, 19U}, "WaterPump", LoadPower{35.0F, 55.0F}, 4U, LoadMode::Fixed::OFF);

    LoadFilter loadFilter;
    loadFilter.addLoad(pump);

    reportCheck("Fixed::OFF Load is counted in the Fixed OFF collection",
                loadFilter.getNumberOfFixedOffLoads() == 1U);
    reportCheck("Fixed::OFF Load does not appear in Fixed ON",
                loadFilter.getNumberOfFixedOnLoads() == 0U);
    reportCheck("Fixed::OFF Load does not appear in Auto candidates",
                loadFilter.getNumberOfAutoCandidateLoads() == 0U);
    reportCheck("getFixedOffLoad(0) returns the same Load object (pointer identity)",
                loadFilter.getFixedOffLoad(0U) == &pump);
}

void testAutoClassification() {
    printSection("TEST 3 - AUTO ON AND AUTO OFF ARE BOTH CANDIDATES");

    Load fan(Load::Id{DEMO_MAC, 18U}, "Fan", LoadPower{18.0F, 25.0F}, 3U, LoadMode::Auto::ON);
    Load chargingPort(Load::Id{DEMO_MAC, 21U}, "ChargingPort", LoadPower{15.0F, 15.0F}, 5U, LoadMode::Auto::OFF);

    LoadFilter loadFilter;
    loadFilter.addLoad(fan);
    loadFilter.addLoad(chargingPort);

    reportCheck("Auto::ON Load is classified as an Auto candidate",
                loadFilter.getNumberOfAutoCandidateLoads() == 2U);
    reportCheck("Auto candidates are not classified as Fixed ON",
                loadFilter.getNumberOfFixedOnLoads() == 0U);
    reportCheck("Auto candidates are not classified as Fixed OFF",
                loadFilter.getNumberOfFixedOffLoads() == 0U);
    reportCheck("getAutoCandidateLoad(0) returns the first Auto Load added (pointer identity)",
                loadFilter.getAutoCandidateLoad(0U) == &fan);
    reportCheck("getAutoCandidateLoad(1) returns the second Auto Load added (pointer identity)",
                loadFilter.getAutoCandidateLoad(1U) == &chargingPort);
}

void testMixedClassificationAndReset() {
    printSection("TEST 4 - MIXED CLASSIFICATION AND RESET");

    Load light(Load::Id{DEMO_MAC, 16U}, "Light", LoadPower{12.0F, 12.0F}, 1U, LoadMode::Fixed::ON);
    Load router(Load::Id{DEMO_MAC, 17U}, "Router", LoadPower{10.0F, 10.0F}, 2U, LoadMode::Fixed::ON);
    Load pump(Load::Id{DEMO_MAC, 19U}, "WaterPump", LoadPower{35.0F, 55.0F}, 4U, LoadMode::Fixed::OFF);
    Load fan(Load::Id{DEMO_MAC, 18U}, "Fan", LoadPower{18.0F, 25.0F}, 3U, LoadMode::Auto::ON);

    LoadFilter loadFilter;
    loadFilter.addLoad(light);
    loadFilter.addLoad(router);
    loadFilter.addLoad(pump);
    loadFilter.addLoad(fan);

    reportCheck("Two Fixed::ON Loads are both classified", loadFilter.getNumberOfFixedOnLoads() == 2U);
    reportCheck("One Fixed::OFF Load is classified", loadFilter.getNumberOfFixedOffLoads() == 1U);
    reportCheck("One Auto Load is classified", loadFilter.getNumberOfAutoCandidateLoads() == 1U);

    loadFilter.reset();

    reportCheck("reset() clears the Fixed ON collection", loadFilter.getNumberOfFixedOnLoads() == 0U);
    reportCheck("reset() clears the Fixed OFF collection", loadFilter.getNumberOfFixedOffLoads() == 0U);
    reportCheck("reset() clears the Auto candidate collection", loadFilter.getNumberOfAutoCandidateLoads() == 0U);
    reportCheck("getFixedOnLoad(0) returns nullptr after reset()", loadFilter.getFixedOnLoad(0U) == nullptr);
}

void testOutOfRangeLookups() {
    printSection("TEST 5 - OUT OF RANGE LOOKUPS");

    LoadFilter loadFilter;

    reportCheck("getFixedOnLoad() on an empty filter returns nullptr", loadFilter.getFixedOnLoad(0U) == nullptr);
    reportCheck("getFixedOffLoad() on an empty filter returns nullptr", loadFilter.getFixedOffLoad(0U) == nullptr);
    reportCheck("getAutoCandidateLoad() on an empty filter returns nullptr", loadFilter.getAutoCandidateLoad(0U) == nullptr);
}

} // namespace

int main() {
    testFixedOnClassification();
    testFixedOffClassification();
    testAutoClassification();
    testMixedClassificationAndReset();
    testOutOfRangeLookups();

    std::printf("\n======================================================================\n");
    std::printf("RESULTS: %zu passed, %zu failed\n", passedChecks, failedChecks);
    std::printf("======================================================================\n");

    return failedChecks == 0U ? 0 : 1;
}
