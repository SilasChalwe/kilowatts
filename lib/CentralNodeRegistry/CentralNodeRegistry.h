/**
 * @file CentralNodeRegistry.h
 * @brief Declares Central's planning-time view of every known Node.
 *
 * The Central Node receives NodeReportPacket/LoadReportPacket reports over
 * ESP-NOW. Those packets are a transport representation: raw bytes
 * describing what one Node currently looks like. CentralNodeRegistry
 * converts each received report into the real kilowatts::Node /
 * kilowatts::Load domain objects used by the rest of the system, and keeps
 * one up-to-date copy of every Node Central currently knows about,
 * including Central's own local Node.
 *
 * A Load's identity is its owning Node's MAC address plus its relay pin
 * (see Load::Id). CentralNodeRegistry always takes that owning MAC
 * address from the report's own NodeReportPacket::nodeMacAddress field, never
 * from the immediate ESP-NOW sender of the packet. A Node that forwards a
 * report on behalf of one of its own descendants must not have that
 * descendant's Loads reassigned to itself.
 *
 * CentralNodeRegistry holds and updates this data only. It does not
 * perform ESP-NOW discovery, RSSI or hop-count calculation, battery State
 * of Charge or available-power calculation, Best-First Search, MQTT
 * communication, or relay control. Those responsibilities belong to other
 * modules and later phases.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 8 May 2026
 */

#ifndef KILOWATTS_CENTRAL_NODE_REGISTRY_H
#define KILOWATTS_CENTRAL_NODE_REGISTRY_H

#include "Node.h"
#include "NodeReportPackets.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kilowatts {


class CentralNodeRegistry {

public:

    using MacAddress = Node::MacAddress;


    /**
     * One Branch's configuration as last reported by the Node that owns
     * it: the relay pin identifying the Branch inside that Node (paired
     * with the enclosing PlanningNode's own MAC address, this is exactly
     * BestFirstSearch::BranchId), and I_branch,max, the safe configured
     * current limit for that circuit (Section 4.5.1) — never the relay's
     * printed contact rating, always the configured safe value reported
     * by the owning Node. Committed power is not stored here: that is a
     * per-planning-cycle value BestFirstSearch itself owns and evolves
     * (BestFirstSearch::getBranchCommittedPowerWatts()), not persistent
     * registry state.
     */
    struct BranchConfiguration {
        std::uint8_t relayPin;
        float maximumCurrentAmps;
    };


    /**
     * Bundles one known Node's domain object together with the network
     * topology Central currently has for it, so a caller does not need a
     * second lookup to relate a Node's Loads to how Central currently
     * reaches it.
     */
    struct PlanningNode {

        /** The Node's real MAC address and the Loads it owns. */
        Node node;

        /** Human-readable name carried in the Node's report (e.g. "Sitting Room"). */
        std::string nodeName;

        /**
         * The immediate ESP32 this Node sends toward so a message keeps
         * moving toward Central. Not meaningful for Central's own entry,
         * since Central is the root and has no Next Hop of its own.
         */
        MacAddress nextHopToCentralMacAddress;

        /**
         * Number of ESP32-to-ESP32 communication steps between this Node
         * and Central. Always 0 for Central's own entry.
         */
        std::uint16_t hopCountToCentral;

        /** True only for the single entry representing Central itself. */
        bool isCentralNode;

        /**
         * Branch configuration for every relay pin this Node has ever
         * reported, in the order first seen. Central's own local Loads
         * (added through addLocalCentralNode()) carry no Branch
         * configuration here — the caller registers those directly with
         * whatever policy Central uses for its own local relays.
         */
        std::vector<BranchConfiguration> branchConfigurations;

        /**
         * millis()-style timestamp of the last NODE_REPORT actually
         * received from this Node (see applyNodeReport()'s
         * nowMilliseconds parameter). Used by the caller to decide
         * whether a Node should be treated as online (see
         * TopologyTree::buildTreeJson()) or as a Node Link Loss fault.
         * Always the timestamp of the most recent addLocalCentralNode()
         * call for Central's own entry, since Central is always "seen".
         */
        std::uint32_t lastSeenMilliseconds;
    };


    CentralNodeRegistry();


    /**
     * Registers or refreshes Central's own Node.
     *
     * Central has no Next Hop to Central and a Hop Count of 0 because it
     * is the root of the distribution tree, not a Node reached through
     * another Node.
     */
    void addLocalCentralNode(
        const std::string& nodeName,
        const Node& centralNode,
        std::uint32_t nowMilliseconds
    );


    /**
     * Converts one received NodeReportPacket, and the LoadReportPackets it
     * carries, into this registry's Node/Load domain objects.
     *
     * The first report for a given Node MAC address creates a new
     * PlanningNode. Every later report for the same MAC address updates
     * that PlanningNode's topology fields and updates the matching Load
     * for each LoadReportPacket (found by relay pin) rather than creating
     * a duplicate, so the same physical Load keeps its identity across
     * repeated reports.
     */
    void applyNodeReport(const NodeReportPacket& packet, std::uint32_t nowMilliseconds);


    /** Returns how many Nodes are currently known, including Central. */
    std::size_t getNumberOfNodes() const;


    /**
     * Returns the Node stored at the given position, or nullptr when the
     * position does not exist.
     *
     * The returned pointer is only valid until the next call to
     * applyNodeReport() or addLocalCentralNode(), either of which may add
     * a new Node and reallocate the underlying storage.
     */
    const PlanningNode* getNode(std::size_t nodeIndex) const;


    /**
     * Returns the known Node with the given MAC address, or nullptr when
     * no such Node has been registered yet.
     *
     * The same pointer-validity rule as getNode() applies.
     */
    const PlanningNode* findNodeByMacAddress(const MacAddress& macAddress) const;


    /**
     * Returns I_branch,max for the Branch {macAddress, relayPin}, or
     * false when that Node or that relay pin's Branch configuration is
     * not currently known.
     */
    bool findBranchMaximumCurrentAmps(
        const MacAddress& macAddress,
        std::uint8_t relayPin,
        float& maximumCurrentAmps
    ) const;


    /**
     * Registers/updates Branch configuration for one of Central's own
     * local relay pins. Central's local Loads are added directly through
     * addLocalCentralNode() rather than an ESP-NOW NodeReportPacket, so
     * they need their own explicit way to set I_branch,max.
     *
     * Rejected when no local central Node has been registered yet via
     * addLocalCentralNode(), or maximumCurrentAmps is not finite/positive.
     */
    bool setLocalCentralBranchMaximumCurrentAmps(
        std::uint8_t relayPin,
        float maximumCurrentAmps
    );


    /**
     * Returns a mutable pointer to the Load identified by {macAddress,
     * relayPin}, or nullptr when no such Node/Load is currently known.
     * Used by the caller (an MQTT load-command handler, or applying
     * LoadConfigurationStore's persisted overrides) to update a Load's
     * user configuration in place. The same pointer-validity rule as
     * getNode() applies: valid only until the next applyNodeReport() or
     * addLocalCentralNode() call.
     */
    Load* findMutableLoad(const MacAddress& macAddress, std::uint8_t relayPin);


private:

    /**
     * Non-const lookup used internally so applyNodeReport() and
     * addLocalCentralNode() can update an existing entry in place.
     */
    PlanningNode* findMutablePlanningNode(const MacAddress& macAddress);


    /**
     * Every Node Central currently knows about, in the order first seen.
     */
    std::vector<PlanningNode> planningNodes_;
};


} // namespace kilowatts

#endif // KILOWATTS_CENTRAL_NODE_REGISTRY_H
