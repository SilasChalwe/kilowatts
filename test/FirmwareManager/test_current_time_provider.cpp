/**
 * @file test_current_time_provider.cpp
 * @brief Host-native correctness tests for CurrentTimeProvider's
 *        hardware-free logic: manual date/time validation, the default
 *        (nothing-established-yet) state, and Time Mode/Time Source
 *        bookkeeping during setTimeMode().
 *
 * This targets exactly the part of CurrentTimeProvider.cpp that is always
 * compiled (host and ESP32 alike): isManualDateTimeValid(), the
 * TimeMode/TimeSource in-memory bookkeeping, and the "nothing has ever
 * been established" default state. It never asserts on a real NTP sync,
 * a real applied system-clock change, or real NVS persistence: those are
 * compiled only under ESP_PLATFORM and can only be verified on the ESP32
 * target with real internet connectivity and real flash — see the module
 * README's "On-device diagnostic path" section. Two behaviours in
 * particular cannot be host-tested at all and are called out explicitly
 * rather than mocked:
 *
 * - That a late/in-flight SNTP synchronization event is ignored once Time
 *   Mode has changed away from AUTOMATIC. This is enforced inside the
 *   private, ESP_PLATFORM-only handleTimeSynchronizationEvent() callback,
 *   which never runs (and is compiled to an empty stub) on a host build.
 *   Verified by code inspection of that guard, plus real hardware testing
 *   per the README.
 * - That a real NTP synchronization actually sets Time Source to NTP.
 *   This file does confirm the *negative* property host-side (Source
 *   never becomes NTP without a real synchronization ever happening), but
 *   the positive "a real sync sets it" claim needs real internet
 *   connectivity to verify.
 *
 * This file uses a standard host int main(), not an ESP-IDF app_main(), so
 * it can be compiled and run by run_cpp_test.sh's plain g++ invocation —
 * matching test/BestFirstSearch/test_best_first_search.cpp.
 */

#include "CurrentTimeProvider.h"

#include <cstdio>
#include <ctime>

using kilowatts::CurrentTimeProvider;
using kilowatts::ManualDateTime;
using kilowatts::TimeMode;
using kilowatts::TimeSource;

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

ManualDateTime makeDateTime(
    std::uint16_t year, std::uint8_t month, std::uint8_t day,
    std::uint8_t hour = 0U, std::uint8_t minute = 0U, std::uint8_t second = 0U)
{
    return ManualDateTime{year, month, day, hour, minute, second};
}

void testValidManualDateTimeAccepted() {
    printSection("TEST 1 - VALID MANUAL DATE/TIME IS ACCEPTED");

    reportCheck("An ordinary valid date/time is accepted",
                CurrentTimeProvider::isManualDateTimeValid(makeDateTime(2026U, 8U, 13U, 14U, 30U, 0U)));

    reportCheck("The minimum supported year (2000) is accepted",
                CurrentTimeProvider::isManualDateTimeValid(makeDateTime(2000U, 1U, 1U, 0U, 0U, 0U)));

    reportCheck("The maximum supported year (2099) is accepted",
                CurrentTimeProvider::isManualDateTimeValid(makeDateTime(2099U, 12U, 31U, 23U, 59U, 59U)));

    reportCheck("29 February on a leap year (2024) is accepted",
                CurrentTimeProvider::isManualDateTimeValid(makeDateTime(2024U, 2U, 29U, 12U, 0U, 0U)));

    reportCheck("31 days in a 31-day month (31 January) is accepted",
                CurrentTimeProvider::isManualDateTimeValid(makeDateTime(2026U, 1U, 31U, 0U, 0U, 0U)));
}

void testImpossibleValuesRejected() {
    printSection("TEST 2 - IMPOSSIBLE VALUES ARE REJECTED, NEVER NORMALIZED");

    reportCheck("Year before the supported range (1999) is rejected",
                !CurrentTimeProvider::isManualDateTimeValid(makeDateTime(1999U, 6U, 15U)));

    reportCheck("Year after the supported range (2100) is rejected",
                !CurrentTimeProvider::isManualDateTimeValid(makeDateTime(2100U, 6U, 15U)));

    reportCheck("Month 0 is rejected", !CurrentTimeProvider::isManualDateTimeValid(makeDateTime(2026U, 0U, 15U)));
    reportCheck("Month 13 is rejected", !CurrentTimeProvider::isManualDateTimeValid(makeDateTime(2026U, 13U, 15U)));

    reportCheck("Day 0 is rejected", !CurrentTimeProvider::isManualDateTimeValid(makeDateTime(2026U, 6U, 0U)));

    reportCheck("30 February is rejected (not silently rolled into March)",
                !CurrentTimeProvider::isManualDateTimeValid(makeDateTime(2026U, 2U, 30U)));

    reportCheck("29 February on a non-leap year (2023) is rejected",
                !CurrentTimeProvider::isManualDateTimeValid(makeDateTime(2023U, 2U, 29U)));

    reportCheck("31 April is rejected (April has 30 days)",
                !CurrentTimeProvider::isManualDateTimeValid(makeDateTime(2026U, 4U, 31U)));

    reportCheck("Hour 24 is rejected", !CurrentTimeProvider::isManualDateTimeValid(makeDateTime(2026U, 6U, 15U, 24U)));
    reportCheck("Minute 60 is rejected", !CurrentTimeProvider::isManualDateTimeValid(makeDateTime(2026U, 6U, 15U, 12U, 60U)));
    reportCheck("Second 60 is rejected", !CurrentTimeProvider::isManualDateTimeValid(makeDateTime(2026U, 6U, 15U, 12U, 0U, 60U)));
}

/*
 * A fresh provider must never claim a valid time it does not have —
 * neither initializeTimeSynchronization() nor setManualCurrentDateTime()
 * have been called yet.
 */
void testDefaultStateHasNoValidTime() {
    printSection("TEST 3 - DEFAULT STATE HAS NO VALID TIME");

    CurrentTimeProvider currentTimeProvider;

    reportCheck("A fresh provider defaults to AUTOMATIC Time Mode",
                currentTimeProvider.getTimeMode() == TimeMode::AUTOMATIC);
    reportCheck("A fresh provider's Time Source is NONE",
                currentTimeProvider.getCurrentTimeSource() == TimeSource::NONE);
    reportCheck("A fresh provider reports current time as not valid",
                !currentTimeProvider.isCurrentTimeValid());

    std::tm localDateTime{};
    reportCheck("getCurrentLocalDateTime() fails rather than fabricating a time",
                !currentTimeProvider.getCurrentLocalDateTime(localDateTime));

    std::uint8_t hour = 0U;
    std::uint8_t minute = 0U;
    std::uint8_t second = 0U;
    reportCheck("getCurrentLocalHour() fails on a fresh provider", !currentTimeProvider.getCurrentLocalHour(hour));
    reportCheck("getCurrentLocalMinute() fails on a fresh provider", !currentTimeProvider.getCurrentLocalMinute(minute));
    reportCheck("getCurrentLocalSecond() fails on a fresh provider", !currentTimeProvider.getCurrentLocalSecond(second));
}

/*
 * On a host build there is no real NVS/SNTP, so setTimeMode() honestly
 * reports failure (it could not persist or (re)start real hardware) —
 * but the in-memory Time Mode still changes.
 */
void testTimeModeBookkeeping() {
    printSection("TEST 4 - TIME MODE BOOKKEEPING: AUTOMATIC -> MANUAL -> AUTOMATIC");

    CurrentTimeProvider currentTimeProvider;

    reportCheck("Selecting the already-current mode (AUTOMATIC) is a no-op that reports success",
                currentTimeProvider.setTimeMode(TimeMode::AUTOMATIC));
    reportCheck("...and Time Mode is still AUTOMATIC", currentTimeProvider.getTimeMode() == TimeMode::AUTOMATIC);

    const bool switchedToManual = currentTimeProvider.setTimeMode(TimeMode::MANUAL);
    reportCheck("Switching to MANUAL on a host build honestly reports failure (no real NVS to persist to)",
                !switchedToManual);
    reportCheck("...but Time Mode still becomes MANUAL in memory",
                currentTimeProvider.getTimeMode() == TimeMode::MANUAL);
    reportCheck("...and Time Source is reset to NONE by the mode change",
                currentTimeProvider.getCurrentTimeSource() == TimeSource::NONE);

    const bool switchedToAutomatic = currentTimeProvider.setTimeMode(TimeMode::AUTOMATIC);
    reportCheck("Switching back to AUTOMATIC on a host build also honestly reports failure",
                !switchedToAutomatic);
    reportCheck("...but Time Mode still becomes AUTOMATIC in memory",
                currentTimeProvider.getTimeMode() == TimeMode::AUTOMATIC);
    reportCheck("Time Source is not claimed as NTP without a real synchronization ever happening",
                currentTimeProvider.getCurrentTimeSource() != TimeSource::NTP);
}

/*
 * Even a perfectly valid ManualDateTime must not be reported as applied
 * on a host build, since there is no real system clock of a Node to set.
 */
void testHostBuildNeverFabricatesManualApplication() {
    printSection("TEST 5 - HOST BUILD NEVER FABRICATES A HARDWARE-APPLIED MANUAL TIME");

    CurrentTimeProvider currentTimeProvider;

    const ManualDateTime validDateTime = makeDateTime(2026U, 8U, 13U, 10U, 0U, 0U);
    reportCheck("The chosen fixture date/time is itself valid (sanity check)",
                CurrentTimeProvider::isManualDateTimeValid(validDateTime));

    const bool applied = currentTimeProvider.setManualCurrentDateTime(validDateTime);
    reportCheck("setManualCurrentDateTime() with a valid value still reports failure on a host build",
                !applied);
    reportCheck("Time Source stays NONE — a host build never claims MANUAL without really applying it",
                currentTimeProvider.getCurrentTimeSource() == TimeSource::NONE);

    const ManualDateTime invalidDateTime = makeDateTime(2026U, 2U, 30U);
    reportCheck("setManualCurrentDateTime() also rejects an invalid value",
                !currentTimeProvider.setManualCurrentDateTime(invalidDateTime));

    std::tm lastManualDateTime{};
    reportCheck("getLastConfiguredManualDateTime() reports false with no real NVS to read",
                !currentTimeProvider.getLastConfiguredManualDateTime(lastManualDateTime));
}

/*
 * There is no real NVS on a host build, so setup honestly fails rather
 * than pretending initialization succeeded.
 */
void testInitializeOnHostBuild() {
    printSection("TEST 6 - initializeTimeSynchronization() ON A HOST BUILD");

    CurrentTimeProvider currentTimeProvider;

    reportCheck("initializeTimeSynchronization() reports failure on a host build (no real NVS)",
                !currentTimeProvider.initializeTimeSynchronization());
    reportCheck("Time Mode still defaults to AUTOMATIC after the failed attempt",
                currentTimeProvider.getTimeMode() == TimeMode::AUTOMATIC);
    reportCheck("Current time is still not valid after the failed attempt",
                !currentTimeProvider.isCurrentTimeValid());
}

} // namespace

int main() {
    testValidManualDateTimeAccepted();
    testImpossibleValuesRejected();
    testDefaultStateHasNoValidTime();
    testTimeModeBookkeeping();
    testHostBuildNeverFabricatesManualApplication();
    testInitializeOnHostBuild();

    std::printf("\n======================================================================\n");
    std::printf("RESULTS: %zu passed, %zu failed\n", passedChecks, failedChecks);
    std::printf("======================================================================\n");

    return failedChecks == 0U ? 0 : 1;
}
