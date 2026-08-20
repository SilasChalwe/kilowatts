/**
 * @file NodeRegistryJson.h
 * @brief Dashboard node/branch lists and relay-pin availability.
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
    static std::string buildStateNodesJson(
        const NodeCommissioningRegistry& commissioningRegistry,
        const CentralNodeRegistry& centralNodeRegistry,
        std::uint32_t schemaVersion,
        std::uint32_t nowMilliseconds,
        std::uint32_t onlineTimeoutMilliseconds);

    static std::string buildConfigNodesJson(
        const NodeCommissioningRegistry& commissioningRegistry,
        const CentralNodeRegistry& centralNodeRegistry,
        std::uint32_t schemaVersion);

private:
    static void appendMacAddressJson(std::string& out, const MacAddress& macAddress);
    static const char* syncStateText(NodeCommissioningRegistry::SyncState syncState);
};

} // namespace kilowatts

#endif // KILOWATTS_NODE_REGISTRY_JSON_H
