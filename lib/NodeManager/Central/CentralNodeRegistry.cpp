#include "CentralNodeRegistry.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace kilowatts {

namespace {

std::size_t boundedLength(const char* value, std::size_t capacity)
{
    std::size_t length = 0U;
    while (length < capacity && value[length] != '\0') {
        ++length;
    }
    return length;
}

bool validMode(std::uint8_t mode)
{
    return mode == static_cast<std::uint8_t>(LoadMode::Fixed::OFF) ||
           mode == static_cast<std::uint8_t>(LoadMode::Fixed::ON) ||
           mode == static_cast<std::uint8_t>(LoadMode::Auto::OFF) ||
           mode == static_cast<std::uint8_t>(LoadMode::Auto::ON);
}

bool validLoadReport(const LoadReportPacket& packet)
{
    const std::size_t nameLength = boundedLength(packet.name, sizeof(packet.name));
    const float runningWatts = packet.nominalVoltageVolts * packet.nominalCurrentAmps;

    return nameLength > 0U && nameLength < sizeof(packet.name) &&
           validMode(packet.mode) && packet.priority <= 10U &&
           (!packet.scheduleEnabled || (packet.scheduleHour <= 23U && packet.scheduleMinute <= 59U)) &&
           std::isfinite(packet.nominalVoltageVolts) && packet.nominalVoltageVolts > 0.0F &&
           std::isfinite(packet.nominalCurrentAmps) && packet.nominalCurrentAmps > 0.0F &&
           std::isfinite(runningWatts) && runningWatts > 0.0F &&
           std::isfinite(packet.startupWatts) && packet.startupWatts >= runningWatts &&
           packet.availability <= static_cast<std::uint8_t>(LoadHealth::UNAVAILABLE);
}

} // namespace

CentralNodeRegistry::CentralNodeRegistry() : planningNodes_() {}

void CentralNodeRegistry::addLocalCentralNode(
    const std::string& nodeName,
    const Node& centralNode,
    std::uint32_t nowMilliseconds)
{
    PlanningNode* existing = findMutablePlanningNode(centralNode.getMacAddress());
    if (existing == nullptr) {
        planningNodes_.push_back(PlanningNode{
            centralNode,
            nodeName,
            centralNode.getMacAddress(),
            0U,
            true,
            nowMilliseconds});
        return;
    }

    existing->node = centralNode;
    existing->nodeName = nodeName;
    existing->nextHopToCentralMacAddress = centralNode.getMacAddress();
    existing->hopCountToCentral = 0U;
    existing->isCentralNode = true;
    existing->lastSeenMilliseconds = nowMilliseconds;
}

void CentralNodeRegistry::applyNodeReport(const NodeReportPacket& packet, std::uint32_t nowMilliseconds)
{
    if (packet.pageIndex != 0U || packet.totalPages != 1U) {
        return;
    }

    PlanningNode* planningNode = findMutablePlanningNode(packet.nodeMacAddress);
    if (planningNode == nullptr) {
        planningNodes_.push_back(PlanningNode{
            Node(packet.nodeMacAddress),
            std::string(),
            MacAddress{},
            0U,
            false,
            nowMilliseconds});
        planningNode = &planningNodes_.back();
    }

    planningNode->nodeName.assign(
        packet.nodeName,
        boundedLength(packet.nodeName, sizeof(packet.nodeName)));
    planningNode->nextHopToCentralMacAddress = packet.upstreamNodeMacAddress;
    planningNode->hopCountToCentral = packet.hopCountToCentral;
    planningNode->lastSeenMilliseconds = nowMilliseconds;

    std::vector<std::uint8_t> reportedPins;

    for (std::size_t index = 0U;
         index < packet.numberOfLoads && index < MAX_LOADS_PER_NODE_PACKET;
         ++index) {
        const LoadReportPacket& report = packet.loads[index];
        if (!validLoadReport(report)) {
            continue;
        }

        reportedPins.push_back(report.relayPin);
        const float runningWatts = report.nominalVoltageVolts * report.nominalCurrentAmps;
        Load* load = planningNode->node.getLoadByRelayPin(report.relayPin);

        if (load == nullptr) {
            if (!planningNode->node.addLoad(Load(
                    Load::Id{packet.nodeMacAddress, report.relayPin},
                    report.name,
                    LoadPower{runningWatts, report.startupWatts},
                    report.priority,
                    static_cast<LoadMode::Value>(report.mode),
                    LoadElectricalRatings{report.nominalVoltageVolts, report.nominalCurrentAmps}))) {
                continue;
            }
            load = planningNode->node.getLoadByRelayPin(report.relayPin);
        } else {
            load->setName(report.name);
            load->setMode(static_cast<LoadMode::Value>(report.mode));
            load->setPower(LoadPower{runningWatts, report.startupWatts});
            load->setElectricalRatings(LoadElectricalRatings{
                report.nominalVoltageVolts,
                report.nominalCurrentAmps});
            load->setPriority(report.priority);
        }

        if (load == nullptr) {
            continue;
        }

        if (load->isAuto()) {
            load->setSchedule(AutoSchedule{
                report.scheduleEnabled != 0U,
                report.scheduleHour,
                report.scheduleMinute});
        } else {
            load->clearSchedule();
        }

        if (report.confirmedRelayStateValid != 0U) {
            load->setConfirmedRelayState(report.confirmedRelayState != 0U);
        }
        load->setHealth(static_cast<LoadHealth>(report.availability));
    }

    std::vector<std::uint8_t> pinsToRemove;
    for (std::size_t index = 0U; index < planningNode->node.getNumberOfLoads(); ++index) {
        const Load* load = planningNode->node.getLoad(index);
        if (load != nullptr &&
            std::find(reportedPins.begin(), reportedPins.end(), load->getRelayPin()) == reportedPins.end()) {
            pinsToRemove.push_back(load->getRelayPin());
        }
    }

    for (std::uint8_t pin : pinsToRemove) {
        planningNode->node.removeLoadByRelayPin(pin);
    }
}

std::size_t CentralNodeRegistry::getNumberOfNodes() const
{
    return planningNodes_.size();
}

const CentralNodeRegistry::PlanningNode* CentralNodeRegistry::getNode(std::size_t index) const
{
    return index < planningNodes_.size() ? &planningNodes_[index] : nullptr;
}

const CentralNodeRegistry::PlanningNode* CentralNodeRegistry::findNodeByMacAddress(const MacAddress& macAddress) const
{
    for (const PlanningNode& node : planningNodes_) {
        if (node.node.getMacAddress() == macAddress) {
            return &node;
        }
    }
    return nullptr;
}

CentralNodeRegistry::PlanningNode* CentralNodeRegistry::findMutablePlanningNode(const MacAddress& macAddress)
{
    for (PlanningNode& node : planningNodes_) {
        if (node.node.getMacAddress() == macAddress) {
            return &node;
        }
    }
    return nullptr;
}

Load* CentralNodeRegistry::findMutableLoad(const MacAddress& macAddress, std::uint8_t relayPin)
{
    PlanningNode* node = findMutablePlanningNode(macAddress);
    return node != nullptr ? node->node.getLoadByRelayPin(relayPin) : nullptr;
}

bool CentralNodeRegistry::removeLoad(const MacAddress& macAddress, std::uint8_t relayPin)
{
    PlanningNode* node = findMutablePlanningNode(macAddress);
    return node != nullptr && node->node.removeLoadByRelayPin(relayPin);
}

} // namespace kilowatts
