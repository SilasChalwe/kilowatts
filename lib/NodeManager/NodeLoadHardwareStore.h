/**
 * @file NodeLoadHardwareStore.h
 * @brief Persists the loads physically connected to one Node.
 */
#ifndef KILOWATTS_NODE_LOAD_HARDWARE_STORE_H
#define KILOWATTS_NODE_LOAD_HARDWARE_STORE_H

#include "HardwareConfigurationPackets.h"
#include "Load.h"
#include "Node.h"
#include "RelayController.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kilowatts {

class NodeLoadHardwareStore {
public:
    static constexpr std::size_t MAX_CONFIGURED_LOADS = 3U;

    struct LoadConfiguration {
        char name[16];
        std::uint8_t relayPin;
        bool relayActiveHigh;
        LoadMode::Value mode;
        std::uint16_t priority;
        float nominalVoltageVolts;
        float nominalCurrentAmps;
        float startupWatts;
        AutoSchedule schedule;
    };

    NodeLoadHardwareStore();

    bool loadPersisted();
    bool persist() const;

    std::size_t getNumberOfConfigurations() const;
    const LoadConfiguration* getConfiguration(std::size_t index) const;
    const LoadConfiguration* findByRelayPin(std::uint8_t relayPin) const;

    bool isValidNewConfiguration(
        const LoadConfiguration& configuration,
        HardwareConfigurationFailureReason& failureReason) const;

    bool configureNewLoad(
        const LoadConfiguration& configuration,
        RelayController& relays,
        Node& node,
        HardwareConfigurationFailureReason& failureReason);

    /** Drives the load OFF, persists its removal, then releases its relay pin. */
    bool removeLoad(
        std::uint8_t relayPin,
        RelayController& relays,
        Node& node,
        HardwareConfigurationFailureReason& failureReason);

    bool clearAllConfigurations(
        RelayController& relays,
        Node& node,
        HardwareConfigurationFailureReason& failureReason);

    bool applyPersistedConfigurations(
        RelayController& relays,
        Node& node,
        HardwareConfigurationFailureReason& failureReason) const;

private:
    static bool isValidMode(LoadMode::Value mode);
    static bool applyOne(
        const LoadConfiguration& configuration,
        RelayController& relays,
        Node& node,
        HardwareConfigurationFailureReason& failureReason);
    static void rollbackOne(
        const LoadConfiguration& configuration,
        RelayController& relays,
        Node& node);

    std::vector<LoadConfiguration> configurations_;
};

} // namespace kilowatts

#endif // KILOWATTS_NODE_LOAD_HARDWARE_STORE_H
