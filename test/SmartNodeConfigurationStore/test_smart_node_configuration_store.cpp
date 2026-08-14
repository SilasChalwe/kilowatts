/**
 * @file test_smart_node_configuration_store.cpp
 * @brief Host-native tests for Smart Node physical-load configuration.
 *
 * The host has no NVS or GPIO/I2C hardware. That is useful here: a valid
 * configuration reaches the final persistence step, which honestly fails,
 * and the test proves the transaction removes every provisional relay and
 * Load rather than leaving an orphaned channel behind. Smart Nodes have no
 * per-load INA219 in the final hardware design.
 */

#include "SmartNodeConfigurationStore.h"

#include <cstdio>
#include <cstring>

using kilowatts::HardwareConfigurationFailureReason;
using kilowatts::LoadMode;
using kilowatts::Node;
using kilowatts::RelayController;
using kilowatts::SmartNodeConfigurationStore;

namespace {

std::size_t passedChecks = 0U;
std::size_t failedChecks = 0U;

bool check(const char* label, bool passed)
{
    std::printf("%-82s %s\n", label, passed ? "PASS" : "FAIL");
    passed ? ++passedChecks : ++failedChecks;
    return passed;
}

void section(const char* title)
{
    std::printf("\n======================================================================\n%s\n======================================================================\n", title);
}

SmartNodeConfigurationStore::LoadConfiguration validConfiguration()
{
    SmartNodeConfigurationStore::LoadConfiguration configuration{};
    std::snprintf(configuration.name, sizeof(configuration.name), "Garden Pump");
    configuration.relayPin = 16U;
    configuration.relayActiveHigh = false;
    configuration.mode = LoadMode::Auto::OFF;
    configuration.priority = 6U;
    configuration.nominalVoltageVolts = 24.0F;
    configuration.nominalCurrentAmps = 3.0F;
    configuration.branchMaximumCurrentAmps = 5.0F;
    configuration.startupWatts = 120.0F;
    configuration.schedule = kilowatts::AutoSchedule{false, 0U, 0U};
    return configuration;
}

void testShapeValidation()
{
    section("TEST 1 - INSTALLATION FACT VALIDATION");

    SmartNodeConfigurationStore store;
    HardwareConfigurationFailureReason reason = HardwareConfigurationFailureReason::NONE;

    check("A complete physical-load configuration is accepted",
          store.isValidNewConfiguration(validConfiguration(), reason) &&
              reason == HardwareConfigurationFailureReason::NONE);

    auto invalidPriority = validConfiguration();
    invalidPriority.priority = 11U;
    check("Priority above the documented 0-10 range is rejected",
          !store.isValidNewConfiguration(invalidPriority, reason) &&
              reason == HardwareConfigurationFailureReason::INVALID_CONFIGURATION);

    auto invalidSchedule = validConfiguration();
    invalidSchedule.schedule = kilowatts::AutoSchedule{true, 24U, 0U};
    check("An out-of-range enabled schedule is rejected",
          !store.isValidNewConfiguration(invalidSchedule, reason) &&
              reason == HardwareConfigurationFailureReason::INVALID_CONFIGURATION);

    auto invalidPower = validConfiguration();
    invalidPower.startupWatts = 20.0F;
    check("Startup power below the derived nominal running power is rejected",
          !store.isValidNewConfiguration(invalidPower, reason) &&
              reason == HardwareConfigurationFailureReason::INVALID_ELECTRICAL_RATING);

    auto invalidVoltage = validConfiguration();
    invalidVoltage.nominalVoltageVolts = 0.0F;
    check("A missing nominal voltage is rejected",
          !store.isValidNewConfiguration(invalidVoltage, reason) &&
              reason == HardwareConfigurationFailureReason::INVALID_ELECTRICAL_RATING);
}

void testFailureNeverLeavesPartialHardware()
{
    section("TEST 2 - ATOMIC ROLLBACK ON PERSISTENCE FAILURE");

    SmartNodeConfigurationStore store;
    RelayController relays;
    const Node::MacAddress mac{0x10U, 0x20U, 0x30U, 0x40U, 0x50U, 0x60U};
    Node node(mac);
    HardwareConfigurationFailureReason reason = HardwareConfigurationFailureReason::NONE;

    check("Host NVS failure makes configureNewLoad report failure",
          !store.configureNewLoad(validConfiguration(), relays, node, reason) &&
              reason == HardwareConfigurationFailureReason::PERSISTENCE_FAILED);
    check("A failed persist leaves no stored load configuration",
          store.getNumberOfConfigurations() == 0U);
    check("A failed persist removes the provisional relay",
          !relays.isRelayRegistered(16U) && relays.getNumberOfRelays() == 0U);
    check("A failed persist removes the provisional Node Load",
          node.getLoadByRelayPin(16U) == nullptr && node.getNumberOfLoads() == 0U);
}

void testExistingPhysicalConflictsAreRejected()
{
    section("TEST 3 - EXISTING HARDWARE CONFLICTS");

    const Node::MacAddress mac{0x10U, 0x20U, 0x30U, 0x40U, 0x50U, 0x60U};
    HardwareConfigurationFailureReason reason = HardwareConfigurationFailureReason::NONE;

    {
        SmartNodeConfigurationStore store;
        RelayController relays;
        Node node(mac);
        relays.addRelay(RelayController::RelayConfiguration{16U, false, false});

        check("A relay already owned by local hardware is rejected",
              !store.configureNewLoad(validConfiguration(), relays, node, reason) &&
                  reason == HardwareConfigurationFailureReason::DUPLICATE_RELAY_PIN);
        check("A relay conflict does not add a Load",
              node.getNumberOfLoads() == 0U);
    }
}

} // namespace

int main()
{
    std::printf("KILOWATTS SMART NODE CONFIGURATION STORE HOST TEST REPORT\n");
    testShapeValidation();
    testFailureNeverLeavesPartialHardware();
    testExistingPhysicalConflictsAreRejected();

    section("FINAL TEST SUMMARY");
    std::printf("Passed checks: %zu\nFailed checks: %zu\nOVERALL RESULT: %s\n",
                passedChecks, failedChecks, failedChecks == 0U ? "PASS" : "FAIL");
    return failedChecks == 0U ? 0 : 1;
}
