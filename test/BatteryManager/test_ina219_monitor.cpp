/**
 * @file test_ina219_monitor.cpp
 * @brief Host-native correctness tests for INA219Monitor's sensor
 *        registration and lookup logic.
 *
 * This targets exactly the part of INA219Monitor.cpp that is always
 * compiled (host and ESP32 alike): addSensor()'s validation/bookkeeping
 * and the getSensor()/findSensorByI2CAddress()/findSensorByRelayPin()
 * lookups. It never asserts on a real sensor reading: real I2C register
 * communication is compiled only under ESP_PLATFORM and can only be
 * verified on the ESP32 target with a real INA219 connected. This file
 * instead confirms that on a host build (ESP_PLATFORM not defined) every
 * hardware-touching method honestly returns false rather than fabricating
 * a device presence or a measurement.
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

INA219Monitor::INA219SensorConfiguration makeSensorConfiguration(
    std::uint8_t i2cAddress,
    std::uint8_t relayPin,
    float shuntResistanceOhms = 0.1F,
    float maximumExpectedCurrentAmps = 2.0F,
    float emaAlpha = 0.2F)
{
    return INA219Monitor::INA219SensorConfiguration{
        i2cAddress, relayPin, shuntResistanceOhms, maximumExpectedCurrentAmps, emaAlpha
    };
}

void testRegisterOneSensor() {
    printSection("TEST 1 - REGISTER ONE SENSOR");

    INA219Monitor monitor;

    reportCheck("A new monitor starts with zero registered sensors",
                monitor.getNumberOfSensors() == 0U);

    const bool added = monitor.addSensor(makeSensorConfiguration(0x40U, 16U));

    reportCheck("addSensor() accepts one valid sensor configuration", added);
    reportCheck("getNumberOfSensors() becomes 1 after one registration",
                monitor.getNumberOfSensors() == 1U);

    const INA219Monitor::INA219SensorConfiguration* stored = monitor.getSensor(0U);
    reportCheck("getSensor(0) returns the stored configuration", stored != nullptr);

    if (stored != nullptr) {
        reportCheck("Stored configuration keeps the registered I2C address", stored->i2cAddress == 0x40U);
        reportCheck("Stored configuration keeps the registered relay pin", stored->relayPin == 16U);
    }

    reportCheck("getSensor() returns nullptr for an out-of-range index",
                monitor.getSensor(1U) == nullptr);
}

void testRegisterSeveralSensors() {
    printSection("TEST 2 - REGISTER SEVERAL SENSORS");

    INA219Monitor monitor;

    const bool addedFirst = monitor.addSensor(makeSensorConfiguration(0x40U, 16U));
    const bool addedSecond = monitor.addSensor(makeSensorConfiguration(0x41U, 17U));
    const bool addedThird = monitor.addSensor(makeSensorConfiguration(0x44U, 18U));

    reportCheck("First of three sensors is accepted", addedFirst);
    reportCheck("Second of three sensors is accepted", addedSecond);
    reportCheck("Third of three sensors is accepted", addedThird);
    reportCheck("getNumberOfSensors() reflects all three registrations",
                monitor.getNumberOfSensors() == 3U);
}

void testFindSensorByI2CAddress() {
    printSection("TEST 3 - FIND SENSOR BY I2C ADDRESS");

    INA219Monitor monitor;
    monitor.addSensor(makeSensorConfiguration(0x40U, 16U));
    monitor.addSensor(makeSensorConfiguration(0x45U, 19U));

    const INA219Monitor::INA219SensorConfiguration* found = monitor.findSensorByI2CAddress(0x45U);
    reportCheck("findSensorByI2CAddress() finds a registered address", found != nullptr);

    if (found != nullptr) {
        reportCheck("Found sensor carries the matching relay pin", found->relayPin == 19U);
    }

    reportCheck("findSensorByI2CAddress() returns nullptr for an unregistered address",
                monitor.findSensorByI2CAddress(0x4AU) == nullptr);
}

void testFindSensorByRelayPin() {
    printSection("TEST 4 - FIND SENSOR BY RELAY PIN");

    INA219Monitor monitor;
    monitor.addSensor(makeSensorConfiguration(0x40U, 16U));
    monitor.addSensor(makeSensorConfiguration(0x45U, 19U));

    const INA219Monitor::INA219SensorConfiguration* found = monitor.findSensorByRelayPin(16U);
    reportCheck("findSensorByRelayPin() finds a registered relay pin", found != nullptr);

    if (found != nullptr) {
        reportCheck("Found sensor carries the matching I2C address", found->i2cAddress == 0x40U);
    }

    reportCheck("findSensorByRelayPin() returns nullptr for an unregistered relay pin",
                monitor.findSensorByRelayPin(99U) == nullptr);
}

void testRejectDuplicateI2CAddress() {
    printSection("TEST 5 - REJECT DUPLICATE I2C ADDRESS");

    INA219Monitor monitor;
    monitor.addSensor(makeSensorConfiguration(0x40U, 16U));

    const bool secondWithSameAddress = monitor.addSensor(makeSensorConfiguration(0x40U, 17U));

    reportCheck("A second sensor reusing an already-registered I2C address is rejected",
                !secondWithSameAddress);
    reportCheck("The rejected duplicate does not increase the sensor count",
                monitor.getNumberOfSensors() == 1U);
    reportCheck("The relay pin from the rejected duplicate stays unregistered",
                monitor.findSensorByRelayPin(17U) == nullptr);
}

void testRejectDuplicateRelayPin() {
    printSection("TEST 6 - REJECT DUPLICATE RELAY PIN");

    INA219Monitor monitor;
    monitor.addSensor(makeSensorConfiguration(0x40U, 16U));

    const bool secondWithSameRelayPin = monitor.addSensor(makeSensorConfiguration(0x41U, 16U));

    reportCheck("A second sensor reusing an already-registered relay pin is rejected",
                !secondWithSameRelayPin);
    reportCheck("The rejected duplicate does not increase the sensor count",
                monitor.getNumberOfSensors() == 1U);
    reportCheck("The I2C address from the rejected duplicate stays unregistered",
                monitor.findSensorByI2CAddress(0x41U) == nullptr);
}

/*
 * Covers every value INA219Monitor can validate without any hardware: an
 * I2C address outside the INA219's real 0x40-0x4F address space, a
 * non-positive shunt resistance or maximum current, and a shunt/current
 * combination that would exceed the sensor's measurable shunt-voltage
 * range.
 */
void testRejectInvalidConfiguration() {
    printSection("TEST 7 - REJECT INVALID ADDRESS/CONFIGURATION");

    INA219Monitor monitor;

    reportCheck("An I2C address below the INA219 address space (0x3F) is rejected",
                !monitor.addSensor(makeSensorConfiguration(0x3FU, 1U)));

    reportCheck("An I2C address above the INA219 address space (0x50) is rejected",
                !monitor.addSensor(makeSensorConfiguration(0x50U, 2U)));

    reportCheck("A zero shunt resistance is rejected",
                !monitor.addSensor(makeSensorConfiguration(0x40U, 3U, 0.0F, 2.0F)));

    reportCheck("A negative maximum current is rejected",
                !monitor.addSensor(makeSensorConfiguration(0x41U, 4U, 0.1F, -1.0F)));

    /*
     * 1.0 ohm x 1.0 A = 1.0 V, far beyond the +-320 mV full-scale range
     * this module configures the sensor for.
     */
    reportCheck("A shunt/current combination exceeding the +-320 mV range is rejected",
                !monitor.addSensor(makeSensorConfiguration(0x42U, 5U, 1.0F, 1.0F)));

    reportCheck("None of the rejected configurations registered a sensor",
                monitor.getNumberOfSensors() == 0U);

    /*
     * A configuration comfortably inside the measurable range (0.1 ohm
     * shunt, 3.0 A max => 300 mV, under the 320 mV full-scale limit) is
     * valid and must be accepted. 3.2 A is deliberately avoided here: at
     * float precision, 0.1F * 3.2F does not land exactly on 0.32F, so it
     * would make this check depend on IEEE-754 rounding rather than on
     * the validation rule actually being tested.
     */
    reportCheck("A shunt/current combination safely inside the +-320 mV range is accepted",
                monitor.addSensor(makeSensorConfiguration(0x43U, 6U, 0.1F, 3.0F)));

    reportCheck("emaAlpha == 0 is rejected",
                !monitor.addSensor(makeSensorConfiguration(0x44U, 7U, 0.1F, 2.0F, 0.0F)));
    reportCheck("emaAlpha > 1 is rejected",
                !monitor.addSensor(makeSensorConfiguration(0x45U, 8U, 0.1F, 2.0F, 1.5F)));
    reportCheck("emaAlpha == 1 (no smoothing) is accepted",
                monitor.addSensor(makeSensorConfiguration(0x46U, 9U, 0.1F, 2.0F, 1.0F)));
}

/*
 * applyExponentialMovingAverage() is pure, hardware-free math and is
 * always compiled, so it is directly host-testable independent of any
 * real or development reading.
 */
void testExponentialMovingAverageMath() {
    printSection("TEST 8 - EXPONENTIAL MOVING AVERAGE MATH");

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
 * setCalibration()/getCalibration() bookkeeping and applyCalibration()'s
 * math are always compiled; only NVS persistence itself (which always
 * reports failure on a host build, per persistCalibration()) is
 * ESP32-only.
 */
void testCalibrationValidationAndApplication() {
    printSection("TEST 9 - CALIBRATION VALIDATION AND APPLICATION");

    INA219Monitor monitor;
    monitor.addSensor(makeSensorConfiguration(0x40U, 16U));

    INA219Monitor::INA219Calibration identity{};
    reportCheck("A freshly registered sensor starts with identity calibration",
                monitor.getCalibration(0x40U, identity) &&
                identity.voltageOffsetVolts == 0.0F && identity.currentOffsetAmps == 0.0F &&
                identity.currentScaleFactor == 1.0F);

    reportCheck("setCalibration() rejects a non-finite offset",
                !monitor.setCalibration(0x40U, INA219Monitor::INA219Calibration{
                    std::numeric_limits<float>::quiet_NaN(), 0.0F, 1.0F}));
    reportCheck("setCalibration() rejects a zero current scale factor",
                !monitor.setCalibration(0x40U, INA219Monitor::INA219Calibration{0.0F, 0.0F, 0.0F}));
    reportCheck("setCalibration() rejects a negative current scale factor",
                !monitor.setCalibration(0x40U, INA219Monitor::INA219Calibration{0.0F, 0.0F, -1.0F}));
    reportCheck("setCalibration() on an unregistered sensor is rejected",
                !monitor.setCalibration(0x99U, INA219Monitor::INA219Calibration{0.0F, 0.0F, 1.0F}));

    const INA219Monitor::INA219Calibration validCalibration{0.05F, -0.02F, 1.02F};
    reportCheck("setCalibration() accepts a valid calibration", monitor.setCalibration(0x40U, validCalibration));

    INA219Monitor::INA219Calibration restored{};
    reportCheck("getCalibration() returns the calibration just set",
                monitor.getCalibration(0x40U, restored) &&
                std::fabs(restored.voltageOffsetVolts - 0.05F) < 0.0001F &&
                std::fabs(restored.currentOffsetAmps - (-0.02F)) < 0.0001F &&
                std::fabs(restored.currentScaleFactor - 1.02F) < 0.0001F);
}

/*
 * Without ESP_PLATFORM, every hardware-touching method must honestly
 * report failure/absence instead of simulating a device or a reading —
 * even for an otherwise validly registered sensor.
 */
void testHostBuildReportsNoHardware() {
    printSection("TEST 10 - HOST BUILD NEVER FABRICATES HARDWARE (DEVELOPMENT OR PRODUCTION)");

    INA219Monitor monitor;

    const INA219Monitor::I2CBusConfiguration busConfiguration{4U, 5U, 400000U, 0U};
    reportCheck("initializeBus() on a host build reports failure, not a fabricated success",
                !monitor.initializeBus(busConfiguration));

    monitor.addSensor(makeSensorConfiguration(0x40U, 16U));

    reportCheck("isSensorPresent() on a host build never reports a fabricated FOUND",
                !monitor.isSensorPresent(0x40U));

    LoadMeasurements measurements{-1.0F, -1.0F, -1.0F};
    const bool readSucceeded = monitor.readMeasurements(0x40U, measurements);

    reportCheck("readMeasurements() on a host build reports failure, not a fabricated reading",
                !readSucceeded);
    reportCheck("A failed readMeasurements() call leaves the output untouched",
                measurements.voltageVolts == -1.0F &&
                measurements.currentAmps == -1.0F &&
                measurements.powerWatts == -1.0F);

    reportCheck("readMeasurementsForRelayPin() on a host build also reports failure",
                !monitor.readMeasurementsForRelayPin(16U, measurements));

    reportCheck("readMeasurementsForRelayPin() reports failure for an unregistered relay pin",
                !monitor.readMeasurementsForRelayPin(200U, measurements));

    LoadMeasurements filtered{-1.0F, -1.0F, -1.0F};
    reportCheck("readFilteredMeasurements() on a host build reports failure, not a fabricated filtered reading",
                !monitor.readFilteredMeasurements(0x40U, filtered));
    reportCheck("A failed readFilteredMeasurements() call leaves the output untouched",
                filtered.voltageVolts == -1.0F && filtered.currentAmps == -1.0F && filtered.powerWatts == -1.0F);
    reportCheck("getLastMeasurementSource() stays NONE on a host build with no reading ever taken",
                monitor.getLastMeasurementSource(0x40U) == MeasurementSource::NONE);

    /*
     * printDiagnosticReport() must not crash even though it has real
     * sensors registered and no hardware backing them.
     */
    monitor.printDiagnosticReport();
    reportCheck("printDiagnosticReport() completes without crashing on a host build", true);
}


void testMeasurementSourceBookkeeping() {
    printSection("TEST 11 - MEASUREMENT SOURCE BOOKKEEPING");

    INA219Monitor monitor;

    reportCheck("getLastMeasurementSource() for an unregistered/never-read address is NONE",
                monitor.getLastMeasurementSource(0x40U) == MeasurementSource::NONE);

    monitor.addSensor(makeSensorConfiguration(0x40U, 16U));

    reportCheck("A never-read registered sensor still reports MeasurementSource::NONE",
                monitor.getLastMeasurementSource(0x40U) == MeasurementSource::NONE);

    reportCheck("toText(MeasurementSource::NONE) == \"NONE\"",
                std::string(toText(MeasurementSource::NONE)) == "NONE");
    reportCheck("toText(MeasurementSource::HARDWARE) == \"HARDWARE\"",
                std::string(toText(MeasurementSource::HARDWARE)) == "HARDWARE");
}

void testRollbackRemoval() {
    printSection("TEST 12 - PROVISIONING ROLLBACK REMOVAL");

    INA219Monitor monitor;
    monitor.addSensor(makeSensorConfiguration(0x40U, 16U));

    reportCheck("removeSensor() removes a provisionally registered INA219",
                monitor.removeSensor(0x40U));
    reportCheck("A removed INA219 no longer occupies its I2C address or relay pin",
                monitor.getNumberOfSensors() == 0U &&
                monitor.findSensorByI2CAddress(0x40U) == nullptr &&
                monitor.findSensorByRelayPin(16U) == nullptr);
    reportCheck("removeSensor() rejects an unknown I2C address", !monitor.removeSensor(0x41U));
}

} // namespace

int main() {
    testRegisterOneSensor();
    testRegisterSeveralSensors();
    testFindSensorByI2CAddress();
    testFindSensorByRelayPin();
    testRejectDuplicateI2CAddress();
    testRejectDuplicateRelayPin();
    testRejectInvalidConfiguration();
    testExponentialMovingAverageMath();
    testCalibrationValidationAndApplication();
    testHostBuildReportsNoHardware();
    testMeasurementSourceBookkeeping();
    testRollbackRemoval();

    std::printf("\n======================================================================\n");
    std::printf("RESULTS: %zu passed, %zu failed\n", passedChecks, failedChecks);
    std::printf("======================================================================\n");

    return failedChecks == 0U ? 0 : 1;
}
