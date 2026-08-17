/**
 * @file TopologyTree.h
 * @brief Declares JSON construction for the kilowatts/v1/state/tree and
 *        kilowatts/v1/state/loads MQTT topics.
 *
 * TopologyTree's one responsibility: walk CentralNodeRegistry's known
 * Nodes/Loads/Branches and format them into the two JSON shapes the
 * mobile application needs — a nested tree (communication topology:
 * Central at the root, Smart Nodes nested by their real Next-Hop
 * relationship, each Node's electrical Branches as leaves) and a flat
 * per-Load array (every field the application needs to render one Load's
 * card/row without recomputing anything). It does not discover Nodes,
 * does not receive ESP-NOW packets, does not run Best-First Search, and
 * does not decide relay states — it only reads what CentralNodeRegistry
 * and each Load already know and formats it.
 *
 * Communication topology (who forwards through whom, from
 * PlanningNode::nextHopToCentralMacAddress) is kept deliberately distinct
 * from electrical Branches (BestFirstSearch::BranchId, an owning Node's
 * relay pin): a Node's position in the tree comes from the former, while
 * the Branches listed under it come from the latter.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
 */

#ifndef KILOWATTS_TOPOLOGY_TREE_H
#define KILOWATTS_TOPOLOGY_TREE_H

#include "CentralNodeRegistry.h"
#include "NodeCommissioningRegistry.h"

#include <cstdint>
#include <string>

namespace kilowatts {


class TopologyTree {

public:

    /**
     * Builds the retained kilowatts/v1/state/tree JSON payload: Central
     * at the root, every known Node nested under its real communication
     * parent (PlanningNode::nextHopToCentralMacAddress), and every Node's
     * electrical Branches (relay pin + Branch configuration + the Load on
     * it) listed as that Node's leaves.
     *
     * A Node is reported "online": true when
     * (nowMilliseconds - PlanningNode::lastSeenMilliseconds) is within
     * onlineTimeoutMilliseconds; Central's own entry is always online.
     * Guards against a routing-loop-corrupted registry the same way
     * src/central/main.cpp's own tree printer historically did: recursion
     * never descends further than one level per known Node.
     */
    static std::string buildTreeJson(
        const CentralNodeRegistry& registry,
        const NodeCommissioningRegistry& commissioningRegistry,
        std::uint32_t schemaVersion,
        std::uint32_t nowMilliseconds,
        std::uint32_t onlineTimeoutMilliseconds
    );


    /**
     * Builds the flat kilowatts/v1/state/loads JSON payload: one object
     * per Load currently known (across every Node, Central included),
     * carrying every field Section 4 ("Load State Published To MQTT")
     * requires so the mobile application never has to recompute
     * anything.
     */
    static std::string buildLoadsJson(
        const CentralNodeRegistry& registry,
        std::uint32_t schemaVersion
    );


private:

    static void appendMacAddressJson(std::string& out, const Load::MacAddress& macAddress);

    static const char* loadModeText(LoadMode::Value mode);

    static const char* loadHealthText(LoadHealth health);

    static const char* rejectionReasonText(std::uint8_t reason);

    static void appendNodeAndChildren(
        std::string& out,
        const CentralNodeRegistry& registry,
        const NodeCommissioningRegistry& commissioningRegistry,
        const Load::MacAddress& nodeMacAddress,
        std::uint32_t nowMilliseconds,
        std::uint32_t onlineTimeoutMilliseconds,
        std::size_t remainingDepthGuard
    );

    /** Appends `"diagnostics":{...}` (or `"diagnostics":null` when nothing has ever been recorded for nodeMacAddress). */
    static void appendDiagnosticsJson(
        std::string& out,
        const NodeCommissioningRegistry& commissioningRegistry,
        const Load::MacAddress& nodeMacAddress
    );

    static void appendBranchesForNode(
        std::string& out,
        const CentralNodeRegistry::PlanningNode& planningNode
    );

    static void appendLoadJson(std::string& out, const Load& load);
};


} // namespace kilowatts

#endif // KILOWATTS_TOPOLOGY_TREE_H
