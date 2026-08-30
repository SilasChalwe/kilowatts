#include "CentralNodeRegistry.h"

#include <algorithm>
#include <cmath>

namespace kilowatts {

namespace {

std::size_t boundedLength(const char* value, std::size_t capacity)
{
    std::size_t length = 0U;

    while (length < capacity &&
           value[length] != '\0') {
        ++length;
    }

    return length;
}

bool validMode(std::uint8_t mode)
{
    return
        mode == static_cast<std::uint8_t>(LoadMode::Fixed::OFF) ||
        mode == static_cast<std::uint8_t>(LoadMode::Fixed::ON) ||
        mode == static_cast<std::uint8_t>(LoadMode::Auto::OFF) ||
        mode == static_cast<std::uint8_t>(LoadMode::Auto::ON);
}

bool validPowerType(std::uint8_t powerType)
{
    return
        powerType == static_cast<std::uint8_t>(LoadPowerType::AC) ||
        powerType == static_cast<std::uint8_t>(LoadPowerType::DC);
}

bool validSchedule(const LoadReportPacket& packet)
{
    if (!packet.scheduleEnabled) {
        return true;
    }

    if (packet.scheduleStartHour > 23U ||
        packet.scheduleStartMinute > 59U ||
        packet.scheduleEndHour > 23U ||
        packet.scheduleEndMinute > 59U) {
        return false;
    }

    const std::uint16_t startMinutes =
        static_cast<std::uint16_t>(packet.scheduleStartHour) * 60U +
        packet.scheduleStartMinute;

    const std::uint16_t endMinutes =
        static_cast<std::uint16_t>(packet.scheduleEndHour) * 60U +
        packet.scheduleEndMinute;

    return startMinutes != endMinutes;
}

bool validLoadReport(const LoadReportPacket& packet)
{
    const std::size_t nameLength =
        boundedLength(packet.name, sizeof(packet.name));

    return
        nameLength > 0U &&
        nameLength < sizeof(packet.name) &&
        validMode(packet.mode) &&
        validPowerType(packet.powerType) &&
        packet.priority <= 10U &&
        std::isfinite(packet.powerRatingWatts) &&
        packet.powerRatingWatts >= 0.0F &&
        validSchedule(packet);
}

} // namespace


CentralNodeRegistry::CentralNodeRegistry()
    : planningNodes_()
{
}


void CentralNodeRegistry::addLocalCentralNode(
    const std::string& nodeName,
    const Node& centralNode,
    std::uint32_t nowMilliseconds)
{
    PlanningNode* existing =
        findMutablePlanningNode(
            centralNode.getMacAddress());

    if (existing == nullptr) {
        planningNodes_.push_back(
            PlanningNode{
                centralNode,
                nodeName,
                centralNode.getMacAddress(),
                0U,
                true,
                nowMilliseconds,
                PendingNodeReport{}});

        return;
    }

    existing->node = centralNode;
    existing->nodeName = nodeName;
    existing->nextHopToCentralMacAddress =
        centralNode.getMacAddress();
    existing->hopCountToCentral = 0U;
    existing->isCentralNode = true;
    existing->lastSeenMilliseconds = nowMilliseconds;
}


namespace {

/** pagesReceivedMask is a bitmask, so more pages than this cannot be tracked. */
constexpr std::uint8_t MAX_TRACKED_REPORT_PAGES = 8U;

} // namespace


void CentralNodeRegistry::applyNodeReport(
    const NodeReportPacket& packet,
    std::uint32_t nowMilliseconds)
{
    if (packet.totalPages == 0U ||
        packet.totalPages > MAX_TRACKED_REPORT_PAGES ||
        packet.pageIndex >= packet.totalPages) {

        return;
    }

    PlanningNode* planningNode =
        findMutablePlanningNode(
            packet.nodeMacAddress);

    if (planningNode == nullptr) {
        planningNodes_.push_back(
            PlanningNode{
                Node(packet.nodeMacAddress),
                std::string(),
                MacAddress{},
                0U,
                false,
                nowMilliseconds,
                PendingNodeReport{}});

        planningNode = &planningNodes_.back();
    }

    planningNode->nodeName.assign(
        packet.nodeName,
        boundedLength(
            packet.nodeName,
            sizeof(packet.nodeName)));

    planningNode->nextHopToCentralMacAddress =
        packet.upstreamNodeMacAddress;

    planningNode->hopCountToCentral =
        packet.hopCountToCentral;

    planningNode->lastSeenMilliseconds =
        nowMilliseconds;


    PendingNodeReport& pending = planningNode->pendingReport;

    if (pending.sequenceId != packet.reportSequenceId ||
        pending.totalPages != packet.totalPages) {

        pending.sequenceId = packet.reportSequenceId;
        pending.totalPages = packet.totalPages;
        pending.pagesReceivedMask = 0U;
        pending.reportedRelayPins.clear();
    }

    const std::uint8_t pageBit =
        static_cast<std::uint8_t>(1U << packet.pageIndex);

    if ((pending.pagesReceivedMask & pageBit) != 0U) {
        /* Duplicate delivery of a page already applied this sequence. */
        return;
    }

    const std::size_t numberOfLoads =
        std::min<std::size_t>(
            packet.numberOfLoads,
            MAX_LOADS_PER_NODE_PACKET);

    for (std::size_t index = 0U;
         index < numberOfLoads;
         ++index) {

        const LoadReportPacket& report =
            packet.loads[index];

        if (!validLoadReport(report)) {
            continue;
        }

        pending.reportedRelayPins.push_back(report.relayPin);

        Load* load =
            planningNode->node.getLoadByRelayPin(
                report.relayPin);

        if (load == nullptr) {
            const Load created(
                Load::Id{
                    packet.nodeMacAddress,
                    report.relayPin},
                report.name,
                report.powerRatingWatts,
                report.priority,
                static_cast<LoadPowerType>(
                    report.powerType),
                static_cast<LoadMode::Value>(
                    report.mode));

            if (!planningNode->node.addLoad(created)) {
                continue;
            }

            load =
                planningNode->node.getLoadByRelayPin(
                    report.relayPin);

        } else {
            load->setName(report.name);
            load->setMode(
                static_cast<LoadMode::Value>(
                    report.mode));

            (void)load->setPowerRatingWatts(
                report.powerRatingWatts);

            load->setPowerType(
                static_cast<LoadPowerType>(
                    report.powerType));

            load->setPriority(report.priority);
        }

        if (load == nullptr) {
            continue;
        }

        if (load->isAuto()) {
            (void)load->setSchedule(
                AutoSchedule{
                    report.scheduleEnabled != 0U,
                    report.scheduleStartHour,
                    report.scheduleStartMinute,
                    report.scheduleEndHour,
                    report.scheduleEndMinute});
        } else {
            load->clearSchedule();
        }
    }

    pending.pagesReceivedMask =
        static_cast<std::uint8_t>(pending.pagesReceivedMask | pageBit);

    const std::uint8_t allPagesMask =
        static_cast<std::uint8_t>((1U << packet.totalPages) - 1U);

    if (pending.pagesReceivedMask != allPagesMask) {
        /*
         * Pages of this sequence are still in flight - do not prune yet,
         * or a load reported only on a not-yet-arrived page would look
         * removed.
         */
        return;
    }

    std::vector<std::uint8_t> pinsToRemove;

    for (std::size_t index = 0U;
         index < planningNode->node.getNumberOfLoads();
         ++index) {

        const Load* load =
            planningNode->node.getLoad(index);

        if (load == nullptr) {
            continue;
        }

        if (std::find(
                pending.reportedRelayPins.begin(),
                pending.reportedRelayPins.end(),
                load->getRelayPin()) ==
            pending.reportedRelayPins.end()) {

            pinsToRemove.push_back(
                load->getRelayPin());
        }
    }

    for (std::uint8_t relayPin : pinsToRemove) {
        (void)planningNode->node.removeLoadByRelayPin(
            relayPin);
    }

    pending.pagesReceivedMask = 0U;
    pending.reportedRelayPins.clear();
}


std::size_t CentralNodeRegistry::getNumberOfNodes() const
{
    return planningNodes_.size();
}


const CentralNodeRegistry::PlanningNode*
CentralNodeRegistry::getNode(std::size_t index) const
{
    return index < planningNodes_.size()
        ? &planningNodes_[index]
        : nullptr;
}


const CentralNodeRegistry::PlanningNode*
CentralNodeRegistry::findNodeByMacAddress(
    const MacAddress& macAddress) const
{
    for (const PlanningNode& node : planningNodes_) {
        if (node.node.getMacAddress() == macAddress) {
            return &node;
        }
    }

    return nullptr;
}


CentralNodeRegistry::PlanningNode*
CentralNodeRegistry::findMutablePlanningNode(
    const MacAddress& macAddress)
{
    for (PlanningNode& node : planningNodes_) {
        if (node.node.getMacAddress() == macAddress) {
            return &node;
        }
    }

    return nullptr;
}


Load* CentralNodeRegistry::findMutableLoad(
    const MacAddress& macAddress,
    std::uint8_t relayPin)
{
    PlanningNode* node =
        findMutablePlanningNode(macAddress);

    return node != nullptr
        ? node->node.getLoadByRelayPin(relayPin)
        : nullptr;
}


bool CentralNodeRegistry::removeLoad(
    const MacAddress& macAddress,
    std::uint8_t relayPin)
{
    PlanningNode* node =
        findMutablePlanningNode(macAddress);

    return
        node != nullptr &&
        node->node.removeLoadByRelayPin(relayPin);
}

} // namespace kilowatts
