/**
 * @file test_ina219_monitor.cpp
 * @brief Host-native correctness tests for INA219Monitor: the single
 *        battery-bus sensor Central owns (one object, one physical INA219
 *        on Central's battery bus — not a multi-sensor registry).
 *
 * `initializeBus()`, real register I/O and NVS calibration persistence are
 * compiled only under ESP_PLATFORM and can only be verified on the ESP32
 * target with a real bus. This file instead covers everything that is
 * always compiled: sensor-configuration validation, the bench-mode
 * simulated reading path (`USE_SIMULATED_READING` in INA219Monitor.cpp),
 * calibration bookkeeping, and the EMA filter math.
 *
 * This file uses a standard host int main(), not an ESP-IDF app_main(), so
 * it can be compiled and run by run_cpp_test.sh's plain g++ invocation —
 * matching test/BestFirstSearch/test_best_first_search.cpp.
 */

#include "INA219Monitor.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

using kilowatts::INA219Monitor;
using kilowatts::LoadMeasurements;
using kilowatts::MeasurementSource;
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
    std::printf("%-70s %s\n", name, recordResult(passed));
    return passed;
}

void printSection(const char* title) {
    std::printf("\n======================================================================\n");
    std::printf("%s\n", title);
    std::printf("======================================================================\n");
}

INA219Monitor::SensorConfiguration makeSensorConfiguration(
    float shuntResistanceOhms = 0.1F,
    float maximumExpectedCurrentAmps = 2.0F,
    float emaAlpha = 0.2F)
{
    return INA219Monitor::SensorConfiguration{shuntResistanceOhms, maximumExpectedCurrentAmps, emaAlpha};
}

/*
 * Analytic bounds of INA219Monitor.cpp's bench-mode simulatedReading():
 * currentAmps rides BASE_CURRENT_AMPS(3.5) +/- CURRENT_SWING_AMPS(2.5), so
 * it always lands in [1.0, 6.0] A; voltageVolts sags from
 * BASE_VOLTAGE_VOLTS(13.4) by up to VOLTAGE_SAG_VOLTS(0.6) proportionally
 * to that current, landing in [12.8, 13.3] V. These are deliberately loose
 * (not exact-value) checks: the wave's phase is a shared, ever-advancing
 * counter, so no test may assume which sample index it will see.
 */
constexpr float SIMULATED_CURRENT_MIN_AMPS = 0.95F;
constexpr float SIMULATED_CURRENT_MAX_AMPS = 6.05F;
constexpr float SIMULATED_VOLTAGE_MIN_VOLTS = 12.75F;
constexpr float SIMULATED_VOLTAGE_MAX_VOLTS = 13.35F;

void testAcceptsValidConfiguration() {
    printSection("TEST 1 - VALID SENSOR CONFIGURATION IS ACCEPTED");

    INA219Monitor monitor;
    reportCheck("A new monitor starts unconfigured", !monitor.isConfigured());

    const bool configured = monitor.configureSensor(makeSensorConfiguration());
    reportCheck("configureSensor() accepts a valid configuration", configured);
    reportCheck("isConfigured() becomes true after a valid configureSensor()", monitor.isConfigured());
}

/*
 * Covers every value INA219Monitor can validate without any hardware: a
 * non-positive shunt resistance or maximum current, an emaAlpha outside
 * (0, 1], and a shunt/current combination that would exceed the sensor's
 * measurable +-320 mV shunt-voltage range.
 */
void testRejectsInvalidConfiguration() {
    printSection("TEST 2 - REJECT INVALID SENSOR CONFIGURATION");

    INA219Monitor monitor;

    reportCheck("A zero shunt resistance is rejected",
                !monitor.configureSensor(makeSensorConfiguration(0.0F, 2.0F)));

    reportCheck("A negative maximum current is rejected",
                !monitor.configureSensor(makeSensorConfiguration(0.1F, -1.0F)));

    /*
     * 1.0 ohm x 1.0 A = 1.0 V, far beyond the +-320 mV full-scale range
     * this module configures the sensor for.
     */
    reportCheck("A shunt/current combination exceeding the +-320 mV range is rejected",
                !monitor.configureSensor(makeSensorConfiguration(1.0F, 1.0F)));

    reportCheck("emaAlpha == 0 is rejected",
                !monitor.configureSensor(makeSensorConfiguration(0.1F, 2.0F, 0.0F)));
    reportCheck("emaAlpha > 1 is rejected",
                !monitor.configureSensor(makeSensorConfiguration(0.1F, 2.0F, 1.5F)));

    reportCheck("None of the rejected configurations left the monitor configured",
                !monitor.isConfigured());

    reportCheck("emaAlpha == 1 (no smoothing) is accepted",
                monitor.configureSensor(makeSensorConfiguration(0.1F, 2.0F, 1.0F)));

    /*
     * 3.2 A is deliberately avoided here: at float precision, 0.1F * 3.2F
     * does not land exactly on 0.32F, so it would make this check depend
     * on IEEE-754 rounding rather than on the validation rule being tested.
     */
    INA219Monitor another;
    reportCheck("A shunt/current combination safely inside the +-320 mV range is accepted",
                another.configureSensor(makeSensorConfiguration(0.1F, 3.0F)));
}

void testRejectedReconfigureLeavesGoodStateIntact() {
    printSection("TEST 3 - A REJECTED RECONFIGURE DOES NOT CLOBBER AN EXISTING GOOD CONFIGURATION");

    INA219Monitor monitor;
    reportCheck("Initial valid configuration is accepted", monitor.configureSensor(makeSensorConfiguration()));

    reportCheck("A later invalid configureSensor() call is rejected",
                !monitor.configureSensor(makeSensorConfiguration(0.0F, 2.0F)));
    reportCheck("The monitor stays configured after the rejected attempt", monitor.isConfigured());

    LoadMeasurements measurements{};
    reportCheck("readMeasurements() still works using the earlier good configuration",
                monitor.readMeasurements(measurements));
}

/*
 * initializeBus() performs real ESP-IDF I2C bus setup and is compiled only
 * under ESP_PLATFORM; a host build must honestly report failure rather
 * than fabricate a bus.
 */
void testHostBuildHasNoRealI2CBus() {
    printSection("TEST 4 - HOST BUILD HAS NO REAL I2C BUS");

    INA219Monitor monitor;
    const INA219Monitor::I2CBusConfiguration busConfiguration{4U, 5U, 400000U, 0U};
    reportCheck("initializeBus() on a host build reports failure, not a fabricated success",
                !monitor.initializeBus(busConfiguration));
}

/*
 * INA219Monitor.cpp's USE_SIMULATED_READING bench-mode flag is not gated
 * by ESP_PLATFORM, so it is exercised on host builds too. A flat constant
 * reading previously made every bench/demo run look frozen; these checks
 * pin down that it now behaves like a real, moving source instead.
 */
void testSimulatedReadingBehavesLikeARealMovingSource() {
    printSection("TEST 5 - BENCH-MODE SIMULATED READING");

    INA219Monitor monitor;
    LoadMeasurements measurements{-1.0F, -1.0F, -1.0F};

    reportCheck("isSensorPresent() before configuration is false",
                !monitor.isSensorPresent());
    reportCheck("readMeasurements() before configuration reports failure",
                !monitor.readMeasurements(measurements));
    reportCheck("A failed readMeasurements() call leaves the output untouched",
                measurements.voltageVolts == -1.0F &&
                measurements.currentAmps == -1.0F &&
                measurements.powerWatts == -1.0F);

    reportCheck("configureSensor() accepts a valid configuration", monitor.configureSensor(makeSensorConfiguration()));
    reportCheck("isSensorPresent() reports true once configured (bench mode)", monitor.isSensorPresent());

    reportCheck("readMeasurements() succeeds once configured (bench mode)",
                monitor.readMeasurements(measurements));
    reportCheck("getLastMeasurementSource() reports SIMULATED",
                monitor.getLastMeasurementSource() == MeasurementSource::SIMULATED);

    reportCheck("Simulated voltage lands within the modelled 12V-pack range",
                measurements.voltageVolts >= SIMULATED_VOLTAGE_MIN_VOLTS &&
                measurements.voltageVolts <= SIMULATED_VOLTAGE_MAX_VOLTS);
    reportCheck("Simulated current lands within the modelled charge/discharge range",
                measurements.currentAmps >= SIMULATED_CURRENT_MIN_AMPS &&
                measurements.currentAmps <= SIMULATED_CURRENT_MAX_AMPS);
    reportCheck("Power is voltage * current with identity calibration",
                std::fabs(measurements.powerWatts - measurements.voltageVolts * measurements.currentAmps) < 0.01F);

    bool sawADifferentReading = false;
    const float firstVoltage = measurements.voltageVolts;
    const float firstCurrent = measurements.currentAmps;
    for (int i = 0; i < 20; ++i) {
        LoadMeasurements next{};
        if (monitor.readMeasurements(next) &&
            (std::fabs(next.voltageVolts - firstVoltage) > 0.001F ||
             std::fabs(next.currentAmps - firstCurrent) > 0.001F)) {
            sawADifferentReading = true;
            break;
        }
    }
    reportCheck("Repeated readMeasurements() calls are not frozen at one constant value",
                sawADifferentReading);
}

void testFilteredMeasurementsStayPlumbedAndBounded() {
    printSection("TEST 6 - readFilteredMeasurements() STAYS WITHIN THE SAME MODELLED RANGE");

    INA219Monitor monitor;
    monitor.configureSensor(makeSensorConfiguration(0.1F, 2.0F, 0.3F));

    for (int i = 0; i < 5; ++i) {
        LoadMeasurements filtered{};
        reportCheck("readFilteredMeasurements() succeeds once configured", monitor.readFilteredMeasurements(filtered));
        reportCheck("Filtered voltage stays within the modelled range",
                    filtered.voltageVolts >= SIMULATED_VOLTAGE_MIN_VOLTS &&
                    filtered.voltageVolts <= SIMULATED_VOLTAGE_MAX_VOLTS);
        reportCheck("Filtered current stays within the modelled range",
                    filtered.currentAmps >= SIMULATED_CURRENT_MIN_AMPS &&
                    filtered.currentAmps <= SIMULATED_CURRENT_MAX_AMPS);
    }
}

/*
 * applyExponentialMovingAverage() is pure, hardware-free math and is
 * always compiled, so it is directly host-testable independent of any
 * real or simulated reading.
 */
void testExponentialMovingAverageMath() {
    printSection("TEST 7 - EXPONENTIAL MOVING AVERAGE MATH");

    const LoadMeasurements previous{10.0F, 2.0F, 20.0F};
    const LoadMeasurements raw{12.0F, 3.0F, 36.0F};

    const LoadMeasurements halfAlpha = INA219Monitor::applyExponentialMovingAverage(previous, raw, 0.5F);
    reportCheck("alpha=0.5: voltage = 0.5*12 + 0.5*10 = 11.0",
                std::fabs(halfAlpha.voltageVolts - 11.0F) < 0.001F);
    reportCheck("alpha=0.5: current = 0.5*3 + 0.5*2 = 2.5",
                std::fabs(halfAlpha.currentAmps - 2.5F) < 0.001F);
    reportCheck("alpha=0.5: power = 0.5*36 + 0.5*20 = 28.0",
                std::fabs(halfAlpha.powerWatts - 28.0F) < 0.001F);

    const LoadMeasurements alphaOne = INA219Monitor::applyExponentialMovingAverage(previous, raw, 1.0F);
    reportCheck("alpha=1.0 (no smoothing): filtered equals the raw sample exactly",
                std::fabs(alphaOne.voltageVolts - raw.voltageVolts) < 0.001F &&
                std::fabs(alphaOne.currentAmps - raw.currentAmps) < 0.001F &&
                std::fabs(alphaOne.powerWatts - raw.powerWatts) < 0.001F);

    const LoadMeasurements alphaZero = INA219Monitor::applyExponentialMovingAverage(previous, raw, 0.0F);
    reportCheck("alpha clamped to a minimum of 0: filtered stays at the previous value",
                std::fabs(alphaZero.voltageVolts - previous.voltageVolts) < 0.001F);
}

/*
 * setCalibration()/getCalibration() bookkeeping is always compiled; only
 * NVS persistence itself is ESP32-only, and persistCalibration() honestly
 * returns true unconditionally on a host build (there is nothing to
 * persist to), so setCalibration() succeeding here reflects real
 * validation logic, not a fabricated success.
 */
void testCalibrationValidationAndBookkeeping() {
    printSection("TEST 8 - CALIBRATION VALIDATION AND BOOKKEEPING");

    INA219Monitor monitor;

    reportCheck("setCalibration() on an unconfigured sensor is rejected",
                !monitor.setCalibration(INA219Monitor::Calibration{0.0F, 0.0F, 1.0F}));

    monitor.configureSensor(makeSensorConfiguration());

    const INA219Monitor::Calibration identity = monitor.getCalibration();
    reportCheck("A freshly configured sensor starts with identity calibration",
                identity.voltageOffsetVolts == 0.0F && identity.currentOffsetAmps == 0.0F &&
                identity.currentScaleFactor == 1.0F);

    reportCheck("setCalibration() rejects a non-finite offset",
                !monitor.setCalibration(INA219Monitor::Calibration{
                    std::numeric_limits<float>::quiet_NaN(), 0.0F, 1.0F}));
    reportCheck("setCalibration() rejects a zero current scale factor",
                !monitor.setCalibration(INA219Monitor::Calibration{0.0F, 0.0F, 0.0F}));
    reportCheck("setCalibration() rejects a negative current scale factor",
                !monitor.setCalibration(INA219Monitor::Calibration{0.0F, 0.0F, -1.0F}));

    const INA219Monitor::Calibration validCalibration{0.05F, -0.02F, 1.02F};
    reportCheck("setCalibration() accepts a valid calibration", monitor.setCalibration(validCalibration));

    const INA219Monitor::Calibration restored = monitor.getCalibration();
    reportCheck("getCalibration() returns the calibration just set",
                std::fabs(restored.voltageOffsetVolts - 0.05F) < 0.0001F &&
                std::fabs(restored.currentOffsetAmps - (-0.02F)) < 0.0001F &&
                std::fabs(restored.currentScaleFactor - 1.02F) < 0.0001F);
}

/*
 * Uses the analytic bounds of the raw simulated wave (see the
 * SIMULATED_CURRENT_ and SIMULATED_VOLTAGE_ constants above) to confirm
 * calibration is actually applied to readMeasurements() output, without
 * needing to know the exact raw sample any given call lands on.
 */
void testCalibrationAppliesToReadMeasurements() {
    printSection("TEST 9 - CALIBRATION APPLIES TO readMeasurements()");

    INA219Monitor monitor;
    monitor.configureSensor(makeSensorConfiguration());
    reportCheck("Doubling current scale with zero offsets is accepted",
                monitor.setCalibration(INA219Monitor::Calibration{0.0F, 0.0F, 2.0F}));

    LoadMeasurements measurements{};
    reportCheck("readMeasurements() succeeds under the new calibration", monitor.readMeasurements(measurements));

    reportCheck("Calibrated current falls within double the raw simulated range",
                measurements.currentAmps >= 2.0F * SIMULATED_CURRENT_MIN_AMPS &&
                measurements.currentAmps <= 2.0F * SIMULATED_CURRENT_MAX_AMPS);
    reportCheck("Voltage is unaffected by a current-only calibration",
                measurements.voltageVolts >= SIMULATED_VOLTAGE_MIN_VOLTS &&
                measurements.voltageVolts <= SIMULATED_VOLTAGE_MAX_VOLTS);
    reportCheck("Power still equals the calibrated voltage * calibrated current",
                std::fabs(measurements.powerWatts - measurements.voltageVolts * measurements.currentAmps) < 0.01F);
}

void testMeasurementSourceTextAndDiagnosticReport() {
    printSection("TEST 10 - MEASUREMENT SOURCE TEXT AND printDiagnosticReport()");

    INA219Monitor monitor;
    reportCheck("getLastMeasurementSource() on a never-read monitor is NONE",
                monitor.getLastMeasurementSource() == MeasurementSource::NONE);

    reportCheck("toText(MeasurementSource::NONE) == \"NONE\"",
                std::string(toText(MeasurementSource::NONE)) == "NONE");
    reportCheck("toText(MeasurementSource::SIMULATED) == \"SIMULATED\"",
                std::string(toText(MeasurementSource::SIMULATED)) == "SIMULATED");
    reportCheck("toText(MeasurementSource::HARDWARE) == \"HARDWARE\"",
                std::string(toText(MeasurementSource::HARDWARE)) == "HARDWARE");

    /*
     * printDiagnosticReport() logs through ESP_LOGI only under
     * ESP_PLATFORM; on a host build its body is compiled out entirely, so
     * this only confirms it does not crash to call either way.
     */
    monitor.printDiagnosticReport();
    reportCheck("printDiagnosticReport() completes without crashing on a host build", true);
}

} // namespace

int main() {
    testAcceptsValidConfiguration();
    testRejectsInvalidConfiguration();
    testRejectedReconfigureLeavesGoodStateIntact();
    testHostBuildHasNoRealI2CBus();
    testSimulatedReadingBehavesLikeARealMovingSource();
    testFilteredMeasurementsStayPlumbedAndBounded();
    testExponentialMovingAverageMath();
    testCalibrationValidationAndBookkeeping();
    testCalibrationAppliesToReadMeasurements();
    testMeasurementSourceTextAndDiagnosticReport();

    std::printf("\n======================================================================\n");
    std::printf("RESULTS: %zu passed, %zu failed\n", passedChecks, failedChecks);
    std::printf("======================================================================\n");

    return failedChecks == 0U ? 0 : 1;
}
