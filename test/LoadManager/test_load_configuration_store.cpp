/**
 * @file test_load_configuration_store.cpp
 * @brief Host-native correctness tests for LoadConfigurationStore's
 *        in-memory bookkeeping and applyToLoad() behaviour.
 *
 * NVS persistence (loadPersisted()/persist()) is ESP32-only; this file
 * confirms a host build honestly reports failure rather than fabricating
 * durable storage, and exercises everything else directly.
 *
 * This file uses a standard host int main(), not an ESP-IDF app_main(), so
 * it can be compiled and run by run_cpp_test.sh's plain g++ invocation.
 */

#include "LoadConfigurationStore.h"

#include <cstdio>

using kilowatts::AutoSchedule;
using kilowatts::Load;
using kilowatts::LoadConfigurationStore;
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
    std::printf("%-78s %s\n", name, recordResult(passed));
    return passed;
}

void printSection(const char* title) {
    std::printf("\n======================================================================\n");
    std::printf("%s\n", title);
    std::printf("======================================================================\n");
}

const Load::MacAddress NODE_MAC = {0x02, 0x00, 0x00, 0x00, 0xE0, 0x01};

using ConfigurationEntry = LoadConfigurationStore::ConfigurationEntry;

void testSetFindUpsert() {
    printSection("TEST 1 - SET/FIND/UPSERT");

    LoadConfigurationStore store;
    reportCheck("A new store starts with zero entries", store.getNumberOfEntries() == 0U);

    ConfigurationEntry entry{NODE_MAC, 16U, 5U, LoadMode::Auto::ON, AutoSchedule{true, 6U, 30U}};
    reportCheck("setConfiguration() accepts a valid entry", store.setConfiguration(entry));
    reportCheck("getNumberOfEntries() becomes 1", store.getNumberOfEntries() == 1U);

    ConfigurationEntry found{};
    reportCheck("findConfiguration() finds the stored entry",
                store.findConfiguration(NODE_MAC, 16U, found) &&
                found.priority == 5U && found.mode == LoadMode::Auto::ON &&
                found.schedule.enabled && found.schedule.hour == 6U && found.schedule.minute == 30U);

    reportCheck("findConfiguration() returns false for an unregistered relay pin",
                !store.findConfiguration(NODE_MAC, 99U, found));

    /* Re-setting the same {mac, relayPin} updates in place rather than duplicating. */
    ConfigurationEntry updated{NODE_MAC, 16U, 9U, LoadMode::Fixed::ON, AutoSchedule{false, 0U, 0U}};
    reportCheck("setConfiguration() on an existing Load upserts rather than duplicating",
                store.setConfiguration(updated) && store.getNumberOfEntries() == 1U);

    reportCheck("findConfiguration() reflects the updated values",
                store.findConfiguration(NODE_MAC, 16U, found) &&
                found.priority == 9U && found.mode == LoadMode::Fixed::ON && !found.schedule.enabled);
}

void testScheduleValidation() {
    printSection("TEST 2 - SCHEDULE VALIDATION");

    LoadConfigurationStore store;

    ConfigurationEntry invalidHour{NODE_MAC, 16U, 1U, LoadMode::Auto::ON, AutoSchedule{true, 24U, 0U}};
    reportCheck("setConfiguration() rejects an hour of 24", !store.setConfiguration(invalidHour));

    ConfigurationEntry invalidMinute{NODE_MAC, 16U, 1U, LoadMode::Auto::ON, AutoSchedule{true, 6U, 60U}};
    reportCheck("setConfiguration() rejects a minute of 60", !store.setConfiguration(invalidMinute));

    reportCheck("No entries were stored by the rejected calls", store.getNumberOfEntries() == 0U);

    /* A disabled schedule's hour/minute value is irrelevant and must not be validated. */
    ConfigurationEntry disabledScheduleGarbageTime{NODE_MAC, 17U, 1U, LoadMode::Auto::ON, AutoSchedule{false, 99U, 99U}};
    reportCheck("A disabled schedule's out-of-range hour/minute does not block acceptance",
                store.setConfiguration(disabledScheduleGarbageTime));
}

void testApplyToLoad() {
    printSection("TEST 3 - APPLY TO LOAD");

    LoadConfigurationStore store;
    store.setConfiguration(ConfigurationEntry{
        NODE_MAC, 16U, 7U, LoadMode::Auto::ON, AutoSchedule{true, 18U, 0U}
    });

    Load fan(Load::Id{NODE_MAC, 16U}, "Fan", LoadPower{18.0F, 25.0F}, 1U, LoadMode::Auto::OFF);
    reportCheck("applyToLoad() succeeds for a Load with a stored entry", store.applyToLoad(fan));
    reportCheck("applyToLoad() applied the stored priority", fan.getPriority() == 7U);
    reportCheck("applyToLoad() applied the stored mode", fan.getMode() == LoadMode::Auto::ON);
    reportCheck("applyToLoad() applied the stored schedule",
                fan.getSchedule().enabled && fan.getSchedule().hour == 18U && fan.getSchedule().minute == 0U);

    Load unrelatedLoad(Load::Id{NODE_MAC, 200U}, "Unrelated", LoadPower{5.0F, 5.0F}, 3U, LoadMode::Fixed::OFF);
    reportCheck("applyToLoad() returns false for a Load with no stored entry", !store.applyToLoad(unrelatedLoad));
    reportCheck("A Load with no stored entry is left completely unchanged",
                unrelatedLoad.getPriority() == 3U && unrelatedLoad.getMode() == LoadMode::Fixed::OFF);

    /*
     * A stored Auto schedule applied to a Load whose stored mode is Fixed
     * is a harmless no-op, exactly matching Load::setSchedule()'s own
     * rule — never an error, never silently promoting the Load to Auto.
     */
    LoadConfigurationStore fixedWithScheduleStore;
    fixedWithScheduleStore.setConfiguration(ConfigurationEntry{
        NODE_MAC, 21U, 2U, LoadMode::Fixed::ON, AutoSchedule{true, 6U, 0U}
    });
    Load fixedLoad(Load::Id{NODE_MAC, 21U}, "FixedLoad", LoadPower{10.0F, 10.0F}, 1U, LoadMode::Auto::ON);
    reportCheck("applyToLoad() succeeds even when the stored mode is Fixed with a schedule present",
                fixedWithScheduleStore.applyToLoad(fixedLoad));
    reportCheck("The Load ends up Fixed as stored", fixedLoad.getMode() == LoadMode::Fixed::ON);
    reportCheck("No schedule is active on the now-Fixed Load", !fixedLoad.getSchedule().enabled);
}

void testPersistenceHonestOnHostBuild() {
    printSection("TEST 4 - PERSISTENCE IS HONEST ON A HOST BUILD");

    LoadConfigurationStore store;
    store.setConfiguration(ConfigurationEntry{NODE_MAC, 16U, 5U, LoadMode::Auto::ON, AutoSchedule{false, 0U, 0U}});

    reportCheck("persist() on a host build reports failure, not a fabricated success", !store.persist());

    LoadConfigurationStore freshStore;
    reportCheck("loadPersisted() on a host build reports failure, not a fabricated restore",
                !freshStore.loadPersisted());
    reportCheck("A failed loadPersisted() leaves the store with zero entries",
                freshStore.getNumberOfEntries() == 0U);
}

} // namespace

int main() {
    std::printf("KILOWATTS LOADCONFIGURATIONSTORE HOST TEST REPORT\n");

    testSetFindUpsert();
    testScheduleValidation();
    testApplyToLoad();
    testPersistenceHonestOnHostBuild();

    printSection("FINAL TEST SUMMARY");
    std::printf("Passed checks: %zu\n", passedChecks);
    std::printf("Failed checks: %zu\n", failedChecks);
    std::printf("OVERALL RESULT: %s\n", failedChecks == 0U ? "PASS" : "FAIL");

    return failedChecks == 0U ? 0 : 1;
}
