/**
 * @file NodeRegistryJson.h
 * @brief JSON construction for the kilowatts/v1/state/nodes and
 *        kilowatts/v1/config/nodes MQTT topics.
 *
 * Formats NodeCommissioningRegistry's identity/lifecycle records, joined
 * where available with CentralNodeRegistry's route/last-seen data by MAC
 * address, into the two JSON shapes those topics need.
 *
 * state/nodes carries runtime state (online, route, last-seen) alongside
 * identity; config/nodes carries only commissioned identity (name, role,
 * lifecycle/sync status), never route/online/telemetry, matching the
 * state-vs-configuration boundary every other config topic follows.
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
     * PlanningNode exists (a Node discovered by IDENTITY_REPORT but not
     * yet reporting has none - "online", "hopCountToCentral" and
     * "nextHopMac" are JSON null rather than fabricated in that case).
     * Each object also carries "diagnostics" (see
     * NodeCommissioningRegistry::Diagnostics) - free/min-free heap, flash
     * and PSRAM size, silicon revision, CPU core count and frequency,
     * last reset reason, and die temperature where the chip target has a
     * temperature sensor peripheral ("temperatureCelsius" is JSON null on
     * a target that does not, e.g. Central's original ESP32).
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
