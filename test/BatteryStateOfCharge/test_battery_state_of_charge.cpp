/**
 * @file test_battery_state_of_charge.cpp
 * @brief Host-native correctness tests for the coulomb-counting battery
 *        State of Charge estimator (Equation 4.8).
 *
 * update()'s math is plain, hardware-free C++ and always compiled, so it
 * is fully exercised here. Persistence (persist()/the NVS-backed load
 * inside initialize()) is ESP32-only; this file confirms a host build
 * honestly falls back to the supplied default rather than fabricating a
 * persisted value.
 *
 * This file uses a standard host int main(), not an ESP-IDF app_main(), so
 * it can be compiled and run by run_cpp_test.sh's plain g++ invocation.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
 */

#include "BatteryStateOfCharge.h"

#include <cmath>
#include <cstdio>
#include <string>

using kilowatts::BatteryStateOfCharge;

namespace {

std::size_t passedChecks = 0U;
std::size_t failedChecks = 0U;

bool nearValue(float actual, float expected, float tolerance = 0.001F) {
    return std::fabs(actual - expected) <= tolerance;
}

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

void testInitializationValidation() {
    printSection("TEST 1 - INITIALIZATION VALIDATION");

    BatteryStateOfCharge negativeCapacity;
    reportCheck("initialize() rejects a non-positive battery capacity",
                !negativeCapacity.initialize(-1.0F, 80.0F));
    reportCheck("Rejected initialize() leaves the estimator uninitialized", !negativeCapacity.isInitialized());

    BatteryStateOfCharge socOutOfRange;
    reportCheck("initialize() rejects a default SoC above 100",
                !socOutOfRange.initialize(100.0F, 150.0F));

    BatteryStateOfCharge socNegative;
    reportCheck("initialize() rejects a negative default SoC",
                !socNegative.initialize(100.0F, -5.0F));

    BatteryStateOfCharge valid;
    reportCheck("A valid initialize() call succeeds", valid.initialize(100.0F, 80.0F));
    reportCheck("isInitialized() is true after a successful initialize()", valid.isInitialized());

    /*
     * A host build has no persisted SoC to resume from (persist()/the NVS
     * load are ESP32-only), so initialize() must fall back to exactly the
     * supplied default rather than fabricating a persisted value.
     */
    reportCheck("A host build starts from the supplied default SoC (no persisted value exists)",
                nearValue(valid.getStateOfChargePercent(), 80.0F));
}

void testUpdateValidation() {
    printSection("TEST 2 - UPDATE VALIDATION");

    BatteryStateOfCharge notInitialized;
    reportCheck("update() is rejected before initialize()", !notInitialized.update(1.0F, 1.0F));

    BatteryStateOfCharge soc;
    soc.initialize(100.0F, 50.0F);

    reportCheck("update() rejects a non-positive deltaTimeSeconds", !soc.update(1.0F, 0.0F));
    reportCheck("update() rejects a negative deltaTimeSeconds", !soc.update(1.0F, -1.0F));
    reportCheck("A rejected update() leaves the SoC estimate unchanged", nearValue(soc.getStateOfChargePercent(), 50.0F));

    reportCheck("update() accepts a valid discharge current/interval", soc.update(1.0F, 3600.0F));
}

/**
 * TEST 3 - COULOMB COUNTING (Equation 4.8)
 * C_B = 100 Ah, SoC(0) = 50%. Discharging at 10 A for 3600 s (1 hour)
 * removes 10 Ah, i.e. 10% of C_B: SoC(1h) = 50 - 100*10*3600/(3600*100)
 *                                          = 50 - 10 = 40%.
 */
void testCoulombCountingDischarge() {
    printSection("TEST 3 - COULOMB COUNTING: DISCHARGE (Equation 4.8)");

    BatteryStateOfCharge soc;
    soc.initialize(100.0F, 50.0F);

    reportCheck("update() succeeds for a 10A discharge over 1 hour", soc.update(10.0F, 3600.0F));
    reportCheck("SoC after discharging 10Ah from a 100Ah battery at 50% is 40%",
                nearValue(soc.getStateOfChargePercent(), 40.0F));
}

/**
 * TEST 4 - COULOMB COUNTING: CHARGING (negative I_B increases SoC)
 * C_B = 50 Ah, SoC(0) = 40%. Charging at -5 A (i.e. 5A charge current)
 * for 3600 s adds 5 Ah, i.e. 10% of C_B: SoC = 40 + 10 = 50%.
 */
void testCoulombCountingCharge() {
    printSection("TEST 4 - COULOMB COUNTING: CHARGE (negative I_B increases SoC)");

    BatteryStateOfCharge soc;
    soc.initialize(50.0F, 40.0F);

    reportCheck("update() succeeds for a charging (negative) current", soc.update(-5.0F, 3600.0F));
    reportCheck("SoC after charging 5Ah into a 50Ah battery at 40% is 50%",
                nearValue(soc.getStateOfChargePercent(), 50.0F));
}

/**
 * TEST 5 - CLAMPING AT 0% AND 100%
 */
void testClamping() {
    printSection("TEST 5 - CLAMPING AT 0% AND 100%");

    BatteryStateOfCharge dischargesBelowZero;
    dischargesBelowZero.initialize(10.0F, 5.0F);
    /* 20A for 3600s from a 10Ah battery removes 20Ah, far exceeding 10Ah capacity. */
    dischargesBelowZero.update(20.0F, 3600.0F);
    reportCheck("SoC never goes below 0% even with a discharge exceeding the estimate",
                nearValue(dischargesBelowZero.getStateOfChargePercent(), 0.0F));

    BatteryStateOfCharge chargesAboveHundred;
    chargesAboveHundred.initialize(10.0F, 95.0F);
    /* Charging 20Ah into a 10Ah battery would overflow 100% without clamping. */
    chargesAboveHundred.update(-20.0F, 3600.0F);
    reportCheck("SoC never exceeds 100% even with a charge exceeding the estimate",
                nearValue(chargesAboveHundred.getStateOfChargePercent(), 100.0F));
}

/**
 * TEST 6 - MULTIPLE SEQUENTIAL UPDATES ACCUMULATE
 */
void testSequentialUpdatesAccumulate() {
    printSection("TEST 6 - MULTIPLE SEQUENTIAL UPDATES ACCUMULATE");

    BatteryStateOfCharge soc;
    soc.initialize(100.0F, 80.0F);

    /* Ten short 360s (0.1h) steps at 5A: each removes 0.5% of a 100Ah battery. */
    for (int i = 0; i < 10; ++i) {
        reportCheck("Sequential update step succeeds", soc.update(5.0F, 360.0F));
    }

    reportCheck("Ten 0.1-hour, 5A discharge steps from 80% leave SoC at 75%",
                nearValue(soc.getStateOfChargePercent(), 75.0F, 0.01F));
}

/**
 * TEST 7 - PERSISTENCE IS HONEST ON A HOST BUILD
 */
void testPersistenceHonestOnHostBuild() {
    printSection("TEST 7 - PERSISTENCE IS HONEST ON A HOST BUILD");

    BatteryStateOfCharge soc;
    soc.initialize(100.0F, 60.0F);

    reportCheck("persist() on a host build reports failure, not a fabricated success", !soc.persist());

    BatteryStateOfCharge freshInstance;
    reportCheck("A later initialize() still starts from its own default (nothing was actually persisted)",
                freshInstance.initialize(100.0F, 33.0F) &&
                nearValue(freshInstance.getStateOfChargePercent(), 33.0F));
}


/**
 * TEST 8 - VALIDITY AND SOURCE
 */
void testValidityAndSource() {
    printSection("TEST 8 - VALIDITY AND SOURCE");

    BatteryStateOfCharge soc;
    reportCheck("A never-initialize()d instance is invalid (true factory state)", !soc.isValid());
    reportCheck("A never-initialize()d instance reports source UNKNOWN",
                soc.getSource() == kilowatts::StateOfChargeSource::UNKNOWN);

    soc.initialize(100.0F, 80.0F);
    reportCheck("After initialize() with nothing persisted (host build), the estimate is valid", soc.isValid());
    reportCheck("...and its source is INITIAL_COMMISSIONING, not PERSISTED",
                soc.getSource() == kilowatts::StateOfChargeSource::INITIAL_COMMISSIONING);

    reportCheck("update() succeeding moves the source to COULOMB_COUNTING",
                soc.update(1.0F, 60.0F) && soc.getSource() == kilowatts::StateOfChargeSource::COULOMB_COUNTING);
    reportCheck("The estimate remains valid after a real update()", soc.isValid());

    BatteryStateOfCharge rejectedInit;
    reportCheck("A rejected initialize() (invalid capacity) leaves the instance invalid/UNKNOWN",
                !rejectedInit.initialize(-1.0F, 80.0F) && !rejectedInit.isValid() &&
                rejectedInit.getSource() == kilowatts::StateOfChargeSource::UNKNOWN);

    reportCheck("toText(UNKNOWN) == \"UNKNOWN\"",
                std::string(kilowatts::toText(kilowatts::StateOfChargeSource::UNKNOWN)) == "UNKNOWN");
    reportCheck("toText(PERSISTED) == \"PERSISTED\"",
                std::string(kilowatts::toText(kilowatts::StateOfChargeSource::PERSISTED)) == "PERSISTED");
    reportCheck("toText(INITIAL_COMMISSIONING) == \"INITIAL_COMMISSIONING\"",
                std::string(kilowatts::toText(kilowatts::StateOfChargeSource::INITIAL_COMMISSIONING)) == "INITIAL_COMMISSIONING");
    reportCheck("toText(COULOMB_COUNTING) == \"COULOMB_COUNTING\"",
                std::string(kilowatts::toText(kilowatts::StateOfChargeSource::COULOMB_COUNTING)) == "COULOMB_COUNTING");
}

} // namespace

int main() {
    std::printf("KILOWATTS BATTERYSTATEOFCHARGE HOST TEST REPORT\n");
    std::printf("Author: Chalwe Silas\n");
    std::printf("Programme: Final-Year Computer Engineering\n");
    std::printf("Institution: The Copperbelt University\n");

    testInitializationValidation();
    testUpdateValidation();
    testCoulombCountingDischarge();
    testCoulombCountingCharge();
    testClamping();
    testSequentialUpdatesAccumulate();
    testPersistenceHonestOnHostBuild();
    testValidityAndSource();

    printSection("FINAL TEST SUMMARY");
    std::printf("Passed checks: %zu\n", passedChecks);
    std::printf("Failed checks: %zu\n", failedChecks);
    std::printf("OVERALL RESULT: %s\n", failedChecks == 0U ? "PASS" : "FAIL");

    return failedChecks == 0U ? 0 : 1;
}
