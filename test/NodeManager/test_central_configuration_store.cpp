/**
 * Host-native validation tests for CentralConfigurationStore.
 *
 * NVS persistence is intentionally ESP32-only. These checks prove that the
 * installer-supplied battery and safety facts are validated before they are
 * accepted into Central's live configuration.
 */

#include "CentralConfigurationStore.h"

#include <cstdio>

using kilowatts::CentralConfigurationStore;

namespace {

std::size_t passedChecks = 0U;
std::size_t failedChecks = 0U;

bool check(const char* label, bool value)
{
    std::printf("%-76s %s\n", label, value ? "PASS" : "FAIL");
    value ? ++passedChecks : ++failedChecks;
    return value;
}

CentralConfigurationStore::BatterySensorConfiguration validBattery()
{
    return CentralConfigurationStore::BatterySensorConfiguration{
        true, 0x40U, 0.005F, 40.0F, 0.2F, 100.0F, 80.0F, 12.0F
    };
}

CentralConfigurationStore::SafetyPolicy validPolicy()
{
    return CentralConfigurationStore::SafetyPolicy{
        true, 20.0F, 40.0F, 4.0F, 0.9F, 40.0F, 30.0F
    };
}

void testBatteryValidation()
{
    std::printf("\nBATTERY SENSOR VALIDATION\n");
    check("A complete valid battery sensor configuration is accepted",
          CentralConfigurationStore::isValidBatterySensor(validBattery()));

    auto badAddress = validBattery();
    badAddress.i2cAddress = 0x3FU;
    check("A battery sensor outside INA219 I2C range is rejected",
          !CentralConfigurationStore::isValidBatterySensor(badAddress));

    auto badCapacity = validBattery();
    badCapacity.batteryCapacityAmpHours = 0.0F;
    check("A non-positive battery capacity is rejected",
          !CentralConfigurationStore::isValidBatterySensor(badCapacity));

    auto badSoc = validBattery();
    badSoc.initialStateOfChargePercent = 101.0F;
    check("An initial state of charge outside 0-100 is rejected",
          !CentralConfigurationStore::isValidBatterySensor(badSoc));

    auto badVoltage = validBattery();
    badVoltage.nominalVoltageVolts = 0.0F;
    check("A non-positive nominal voltage is rejected",
          !CentralConfigurationStore::isValidBatterySensor(badVoltage));

    auto negativeVoltage = validBattery();
    negativeVoltage.nominalVoltageVolts = -24.0F;
    check("A negative nominal voltage is rejected",
          !CentralConfigurationStore::isValidBatterySensor(negativeVoltage));

    auto customVoltage = validBattery();
    customVoltage.nominalVoltageVolts = 48.0F;
    check("A non-12V nameplate voltage (e.g. a real 48V bank) is accepted - never assumed to be 12V",
          CentralConfigurationStore::isValidBatterySensor(customVoltage));
}

void testSafetyPolicyValidation()
{
    std::printf("\nSAFETY POLICY VALIDATION\n");
    check("A complete valid safety policy is accepted",
          CentralConfigurationStore::isValidSafetyPolicy(validPolicy()));

    auto invertedSoc = validPolicy();
    invertedSoc.warningStateOfChargePercent = 10.0F;
    check("A warning state of charge below the cutoff is rejected",
          !CentralConfigurationStore::isValidSafetyPolicy(invertedSoc));

    auto badFactor = validPolicy();
    badFactor.safetyFactor = 1.1F;
    check("A safety factor above one is rejected",
          !CentralConfigurationStore::isValidSafetyPolicy(badFactor));

    auto noRuntime = validPolicy();
    noRuntime.targetRuntimeHours = 0.0F;
    check("A non-positive target runtime is rejected",
          !CentralConfigurationStore::isValidSafetyPolicy(noRuntime));
}

void testStoreMutationAndHostPersistence()
{
    std::printf("\nSTORE MUTATION AND HOST PERSISTENCE\n");
    CentralConfigurationStore store;
    check("A new store begins with no configured battery sensor",
          !store.getConfiguration().batterySensor.configured);
    check("A new store begins with no configured safety policy",
          !store.getConfiguration().safetyPolicy.configured);
    check("setBatterySensor accepts valid configuration", store.setBatterySensor(validBattery()));
    check("setSafetyPolicy accepts valid configuration", store.setSafetyPolicy(validPolicy()));

    auto invalid = validBattery();
    invalid.emaAlpha = 0.0F;
    check("setBatterySensor rejects invalid replacement", !store.setBatterySensor(invalid));
    check("An invalid replacement leaves the prior valid battery facts intact",
          store.getConfiguration().batterySensor.i2cAddress == 0x40U);

    check("persist reports failure on a host build rather than fabricating success", !store.persist());
    CentralConfigurationStore fresh;
    check("loadPersisted reports failure on a host build rather than fabricating data", !fresh.loadPersisted());
}

} // namespace

int main()
{
    std::printf("KILOWATTS CENTRALCONFIGURATIONSTORE HOST TEST REPORT\n");
    testBatteryValidation();
    testSafetyPolicyValidation();
    testStoreMutationAndHostPersistence();
    std::printf("\nPassed checks: %zu\nFailed checks: %zu\nOVERALL RESULT: %s\n",
                passedChecks, failedChecks, failedChecks == 0U ? "PASS" : "FAIL");
    return failedChecks == 0U ? 0 : 1;
}
