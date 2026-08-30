/**
 * @file CentralNodeRegistry.h
 * @brief Central's current view of every Node and its loads.
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
     * Tracks which pages of the in-progress NodeReportPacket sequence have
     * arrived. applyNodeReport() only prunes loads absent from the report
     * once every page has been seen, so a page lost in transit cannot make
     * a still-configured load look removed.
     */
    struct PendingNodeReport {
        std::uint16_t sequenceId = 0U;
        std::uint8_t totalPages = 0U;
        std::uint8_t pagesReceivedMask = 0U;
        std::vector<std::uint8_t> reportedRelayPins;
    };

    /** One ESP32 Node identified by its MAC address. */
    struct PlanningNode {
        Node node;
        std::string nodeName;
        MacAddress nextHopToCentralMacAddress;
        std::uint16_t hopCountToCentral;
        bool isCentralNode;
        std::uint32_t lastSeenMilliseconds;
        PendingNodeReport pendingReport;
    };

    CentralNodeRegistry();

    void addLocalCentralNode(
        const std::string& nodeName,
        const Node& centralNode,
        std::uint32_t nowMilliseconds);

    void applyNodeReport(const NodeReportPacket& packet, std::uint32_t nowMilliseconds);

    std::size_t getNumberOfNodes() const;
    const PlanningNode* getNode(std::size_t nodeIndex) const;
    const PlanningNode* findNodeByMacAddress(const MacAddress& macAddress) const;
    Load* findMutableLoad(const MacAddress& macAddress, std::uint8_t relayPin);

    /** Removes a load from Central's planning copy after a confirmed hardware removal. */
    bool removeLoad(const MacAddress& macAddress, std::uint8_t relayPin);

private:
    PlanningNode* findMutablePlanningNode(const MacAddress& macAddress);
    std::vector<PlanningNode> planningNodes_;
};

} // namespace kilowatts

#endif // KILOWATTS_CENTRAL_NODE_REGISTRY_H
