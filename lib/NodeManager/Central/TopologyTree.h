/**
 * @file TopologyTree.h
 * @brief Builds the Node tree and flat load list for the dashboard.
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
    static std::string buildTreeJson(
        const CentralNodeRegistry& registry,
        const NodeCommissioningRegistry& commissioningRegistry,
        std::uint32_t schemaVersion,
        std::uint32_t nowMilliseconds,
        std::uint32_t onlineTimeoutMilliseconds);

    static std::string buildLoadsJson(
        const CentralNodeRegistry& registry,
        std::uint32_t schemaVersion);

private:
    static void appendMacAddressJson(std::string& out, const Load::MacAddress& macAddress);
    static const char* loadModeText(LoadMode::Value mode);
    static const char* rejectionReasonText(std::uint8_t reason);

    static void appendLoadJson(
        std::string& out,
        const Load& load,
        const std::string& nodeName);

    static void appendLoadsForNode(
        std::string& out,
        const CentralNodeRegistry::PlanningNode& planningNode);

    static void appendDiagnosticsJson(
        std::string& out,
        const NodeCommissioningRegistry& commissioningRegistry,
        const Load::MacAddress& nodeMacAddress);

    static void appendChildren(
        std::string& out,
        const CentralNodeRegistry& registry,
        const NodeCommissioningRegistry& commissioningRegistry,
        const Load::MacAddress& parentMac,
        std::uint32_t nowMilliseconds,
        std::uint32_t onlineTimeoutMilliseconds,
        std::size_t depthRemaining);
};

} // namespace kilowatts

#endif // KILOWATTS_TOPOLOGY_TREE_H
