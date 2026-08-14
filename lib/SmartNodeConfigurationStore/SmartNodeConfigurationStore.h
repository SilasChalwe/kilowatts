/**
 * @file SmartNodeConfigurationStore.h
 * @brief Runtime, NVS-persisted physical configuration owned by one Smart
 *        Node. A clean flash holds no load or relay configuration.
 *
 * Smart Nodes deliberately have no INA219 in the final hardware design:
 * every load stores installer-entered nominal voltage/current ratings, while
 * Central owns the sole battery-bus INA219.
 */

#ifndef KILOWATTS_SMART_NODE_CONFIGURATION_STORE_H
#define KILOWATTS_SMART_NODE_CONFIGURATION_STORE_H

#include "HardwareConfigurationPackets.h"
#include "Load.h"
#include "Node.h"
#include "RelayController.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kilowatts {

class SmartNodeConfigurationStore {

public:
    /** Matches the maximum one-page reporting capacity until multi-page reports are implemented by Smart Node. */
    static constexpr std::size_t MAX_CONFIGURED_LOADS = 3U;

    struct LoadConfiguration {
        char name[16];
        std::uint8_t relayPin;
        bool relayActiveHigh;
        LoadMode::Value mode;
        std::uint16_t priority;
        /** Installer/nameplate rating; not a live measurement. */
        float nominalVoltageVolts;
        /** Installer/nameplate rating; not a live measurement. */
        float nominalCurrentAmps;
        float branchMaximumCurrentAmps;
        float startupWatts;
        AutoSchedule schedule;
    };

    SmartNodeConfigurationStore();

    bool loadPersisted();
    bool persist() const;

    std::size_t getNumberOfConfigurations() const;
    const LoadConfiguration* getConfiguration(std::size_t index) const;
    const LoadConfiguration* findByRelayPin(std::uint8_t relayPin) const;

    /** Checks only installation facts, never GPIO board safety (the caller owns that board profile check). */
    bool isValidNewConfiguration(const LoadConfiguration& configuration,
                                 HardwareConfigurationFailureReason& failureReason) const;

    /**
     * Configures actual local relay/Load objects and then commits the
     * configuration to NVS. It never replaces an existing channel at runtime
     * because a polarity or rated-electrical change must first be physically
     * decommissioned and verified, not silently overwritten.
     */
    bool configureNewLoad(const LoadConfiguration& configuration,
                          RelayController& relays,
                          Node& node,
                          HardwareConfigurationFailureReason& failureReason);

    /**
     * Safely removes every configured load channel as part of an explicit
     * Node decommission. Every relay must first be driven OFF and verified
     * by GPIO read-back; only then is the empty configuration persisted and
     * the in-memory Load/relay objects removed. This prevents a later
     * recommission from silently reviving an old installation's channels.
     */
    bool clearAllConfigurations(RelayController& relays,
                                Node& node,
                                HardwareConfigurationFailureReason& failureReason);

    /** Restores every valid persisted relay/load channel. */
    bool applyPersistedConfigurations(RelayController& relays,
                                      Node& node,
                                      HardwareConfigurationFailureReason& failureReason) const;

    bool branchMaximumCurrentAmps(std::uint8_t relayPin, float& value) const;

private:
    static bool isValidMode(LoadMode::Value mode);
    static bool applyOne(const LoadConfiguration& configuration,
                         RelayController& relays,
                         Node& node,
                         HardwareConfigurationFailureReason& failureReason);
    static void rollbackOne(const LoadConfiguration& configuration,
                            RelayController& relays,
                            Node& node);

    std::vector<LoadConfiguration> configurations_;
};

} // namespace kilowatts

#endif // KILOWATTS_SMART_NODE_CONFIGURATION_STORE_H
