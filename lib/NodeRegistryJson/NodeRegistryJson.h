/**
 * @file NodeRegistryJson.h
 * @brief Declares JSON construction for the kilowatts/v1/state/nodes and
 *        kilowatts/v1/config/nodes MQTT topics.
 *
 * NodeRegistryJson's one responsibility: format NodeCommissioningRegistry's
 * identity/lifecycle records — joined, where available, with
 * CentralNodeRegistry's route/last-seen data by MAC address — into the two
 * JSON shapes those topics need. It does not discover Nodes, does not
 * perform ESP-NOW communication, does not decide commissioning outcomes,
 * and does not run Best-First Search — it only reads what
 * NodeCommissioningRegistry/CentralNodeRegistry already know and formats
 * it, the same role TopologyTree already fills for state/tree and
 * state/loads.
 *
 * state/nodes carries runtime state (online, route, last-seen) alongside
 * identity, since a monitoring client needs both together; config/nodes
 * carries only commissioned identity (name, role, lifecycle/sync status) —
 * never route/online/telemetry, matching the state-vs-configuration
 * boundary every other "config" topic in this project already follows.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 14 August 2026
 */

#ifndef KILOWATTS_NODE_REGISTRY_JSON_H
#define KILOWATTS_NODE_REGISTRY_JSON_H

#include "CentralNodeRegistry.h"
#include "NodeCommissioningRegistry.h"

#include <cstdint>
#include <string>

namespace kilowatts {


class NodeRegistryJson {

public:

    /**
     * Builds the retained kilowatts/v1/state/nodes JSON payload: one
     * object per record in commissioningRegistry, joined by MAC address
     * with centralNodeRegistry's route/last-seen data where a matching
     * PlanningNode exists yet (a Node discovered by IDENTITY_REPORT but
     * that has not yet sent a NodeReportPacket has none) — "online",
     * "hopCountToCentral" and "nextHopMac" are JSON null rather than
     * fabricated in that case, matching the "keep unavailable values
     * explicit" rule every other diagnostic payload in this project
     * follows.
     */
    static std::string buildStateNodesJson(
        const NodeCommissioningRegistry& commissioningRegistry,
        const CentralNodeRegistry& centralNodeRegistry,
        std::uint32_t schemaVersion,
        std::uint32_t nowMilliseconds,
        std::uint32_t onlineTimeoutMilliseconds
    );


    /**
     * Builds the retained kilowatts/v1/config/nodes JSON payload:
     * commissioned identity only (mac, role, name, lifecycleState,
     * syncState, firmwareVersion, chipModel and relay capabilities) — no
     * route/online/telemetry.
     */
    static std::string buildConfigNodesJson(
        const NodeCommissioningRegistry& commissioningRegistry,
        std::uint32_t schemaVersion
    );


private:

    static void appendMacAddressJson(std::string& out, const MacAddress& macAddress);

    static const char* syncStateText(NodeCommissioningRegistry::SyncState syncState);
};


} // namespace kilowatts

#endif // KILOWATTS_NODE_REGISTRY_JSON_H
