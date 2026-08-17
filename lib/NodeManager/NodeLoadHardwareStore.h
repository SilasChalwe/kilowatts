/**
 * @file NodeLoadHardwareStore.h
 * @brief Runtime, NVS-persisted physical Load/relay configuration owned by
 *        one Node's own locally-attached hardware. A clean flash holds no
 *        load or relay configuration.
 *
 * Used identically by both firmware roles, each with its own instance and
 * NVS storage (separate physical devices, so there is no namespace
 * collision risk): a Smart Node's own directly-wired loads, and Central's
 * own directly-wired loads (distinct from the Loads Central learns about
 * remotely from Smart Nodes over ESP-NOW NodeReportPackets, which never
 * go through this store). Every Load here stores installer-entered nominal
 * voltage/current ratings, never a live per-load measurement — only
 * Central's own battery-bus INA219 (see BatteryManager) reads real
 * current, and only for the whole battery bus, not per Load.
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
    /**
     * On a Smart Node, matches the maximum one-page NODE_REPORT capacity
     * until multi-page reports are implemented (see NodeReportPackets.h);
     * Central's own local loads never travel in a NODE_REPORT, but reuse
     * the same conservative cap for a simple, consistent limit.
     */
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

    NodeLoadHardwareStore();

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

#endif // KILOWATTS_NODE_LOAD_HARDWARE_STORE_H
