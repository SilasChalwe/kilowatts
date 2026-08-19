#include "CentralNodeRegistry.h"

#include "Load.h"

#include <algorithm>
#include <cmath>

// Host test builds have no ESP-IDF headers, so logging compiles out entirely.
#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

namespace kilowatts {

namespace {

std::size_t boundedStringLength(const char* value, std::size_t capacity)
{
    std::size_t length = 0U;
    while (length < capacity && value[length] != '\0') {
        ++length;
    }
    return length;
}


bool isValidMode(std::uint8_t rawMode)
{
    return rawMode == static_cast<std::uint8_t>(LoadMode::Fixed::OFF) ||
           rawMode == static_cast<std::uint8_t>(LoadMode::Fixed::ON) ||
           rawMode == static_cast<std::uint8_t>(LoadMode::Auto::OFF) ||
           rawMode == static_cast<std::uint8_t>(LoadMode::Auto::ON);
}


bool isValidLoadReport(const LoadReportPacket& loadPacket)
{
    const std::size_t nameLength = boundedStringLength(loadPacket.name, sizeof(loadPacket.name));
    const float plannedRunningWatts = loadPacket.nominalVoltageVolts * loadPacket.nominalCurrentAmps;

    return nameLength > 0U && nameLength < sizeof(loadPacket.name) &&
           isValidMode(loadPacket.mode) &&
           loadPacket.priority <= 10U &&
           (!loadPacket.scheduleEnabled ||
            (loadPacket.scheduleHour <= 23U && loadPacket.scheduleMinute <= 59U)) &&
           std::isfinite(loadPacket.nominalVoltageVolts) && loadPacket.nominalVoltageVolts > 0.0F &&
           std::isfinite(loadPacket.nominalCurrentAmps) && loadPacket.nominalCurrentAmps > 0.0F &&
           std::isfinite(plannedRunningWatts) && plannedRunningWatts > 0.0F &&
           std::isfinite(loadPacket.startupWatts) && loadPacket.startupWatts >= plannedRunningWatts &&
           std::isfinite(loadPacket.branchMaximumCurrentAmps) &&
           loadPacket.branchMaximumCurrentAmps > 0.0F &&
           loadPacket.availability <= static_cast<std::uint8_t>(LoadHealth::UNAVAILABLE);
}

} // namespace


#ifdef ESP_PLATFORM
static const char *TAG = "CENTRAL_NODE_REGISTRY";
#endif


CentralNodeRegistry::CentralNodeRegistry()
    : planningNodes_()
{
}


void CentralNodeRegistry::addLocalCentralNode(
    const std::string& nodeName,
    const Node& centralNode,
    std::uint32_t nowMilliseconds)
{
    PlanningNode *planningNode = findMutablePlanningNode(centralNode.getMacAddress());

    if (planningNode == nullptr) {
        planningNodes_.push_back(PlanningNode{
            centralNode,
            nodeName,
            centralNode.getMacAddress(),
            0U,
            true,
            std::vector<BranchConfiguration>(),
            nowMilliseconds
        });
        return;
    }

    /*
     * Central re-registering itself (for example after re-reading its own
     * local Loads) refreshes the existing entry instead of adding a
     * second "Central" row. Branch configuration is left untouched: it is
     * set explicitly through setLocalCentralBranchMaximumCurrentAmps(),
     * not derived from the Node object itself.
     */
    planningNode->node = centralNode;
    planningNode->nodeName = nodeName;
    planningNode->nextHopToCentralMacAddress = centralNode.getMacAddress();
    planningNode->hopCountToCentral = 0U;
    planningNode->isCentralNode = true;
    planningNode->lastSeenMilliseconds = nowMilliseconds;
}


void CentralNodeRegistry::applyNodeReport(const NodeReportPacket& packet, std::uint32_t nowMilliseconds)
{
    /*
     * The fields reserve space for a future paged report protocol, but the
     * current Smart and Central implementations support exactly one page.
     * Rejecting an unsupported partial report prevents it from silently
     * overwriting or pruning an otherwise valid planning topology.
     */
    if (packet.pageIndex != 0U || packet.totalPages != 1U) {
#ifdef ESP_PLATFORM
        ESP_LOGW(TAG, "Ignoring unsupported multi-page Node report page=%u total=%u",
                 static_cast<unsigned int>(packet.pageIndex),
                 static_cast<unsigned int>(packet.totalPages));
#endif
        return;
    }

    PlanningNode *planningNode = findMutablePlanningNode(packet.nodeMacAddress);

    if (planningNode == nullptr) {
        planningNodes_.push_back(PlanningNode{
            Node(packet.nodeMacAddress),
            std::string(),
            MacAddress{},
            0U,
            false,
            std::vector<BranchConfiguration>(),
            nowMilliseconds
        });
        planningNode = &planningNodes_.back();
    }

    planningNode->nodeName = std::string(
        packet.nodeName,
        boundedStringLength(packet.nodeName, sizeof(packet.nodeName)));
    planningNode->nextHopToCentralMacAddress = packet.upstreamNodeMacAddress;
    planningNode->hopCountToCentral = packet.hopCountToCentral;
    planningNode->lastSeenMilliseconds = nowMilliseconds;

    for (std::size_t i = 0; i < packet.numberOfLoads && i < MAX_LOADS_PER_NODE_PACKET; ++i) {
        const LoadReportPacket& loadPacket = packet.loads[i];
        if (!isValidLoadReport(loadPacket)) {
#ifdef ESP_PLATFORM
            ESP_LOGW(TAG, "Ignoring invalid Load report at index %u from Node %s",
                     static_cast<unsigned int>(i), planningNode->nodeName.c_str());
#endif
            continue;
        }

        const float plannedRunningWatts =
            loadPacket.nominalVoltageVolts * loadPacket.nominalCurrentAmps;

        // Lookup by relay pin so a repeated report updates the same Load in place.
        Load *existingLoad = planningNode->node.getLoadByRelayPin(loadPacket.relayPin);

        if (existingLoad == nullptr) {
            const bool added = planningNode->node.addLoad(Load(
                Load::Id{packet.nodeMacAddress, loadPacket.relayPin},
                loadPacket.name,
                LoadPower{
                    plannedRunningWatts,
                    loadPacket.startupWatts
                },
                loadPacket.priority,
                static_cast<LoadMode::Value>(loadPacket.mode)
            ));

            if (!added) {
#ifdef ESP_PLATFORM
                ESP_LOGW(TAG, "Load %s from Node %s was rejected by its Node object",
                         loadPacket.name, packet.nodeName);
#endif
                continue;
            }

            existingLoad = planningNode->node.getLoadByRelayPin(loadPacket.relayPin);
        } else {
            existingLoad->setName(loadPacket.name);
            existingLoad->setMode(static_cast<LoadMode::Value>(loadPacket.mode));
            existingLoad->setPower({
                plannedRunningWatts,
                loadPacket.startupWatts
            });
            existingLoad->setPriority(loadPacket.priority);
        }

        if (existingLoad == nullptr) {
            continue;
        }

        /*
         * setSchedule() is a no-op rejection (not a fault) for a Load
         * that is currently Fixed; mode is always applied above before
         * this call, so isAuto() here reflects this report's own mode.
         */
        existingLoad->setSchedule(AutoSchedule{
            loadPacket.scheduleEnabled != 0U,
            loadPacket.scheduleHour,
            loadPacket.scheduleMinute
        });

        /*
         * Smart Nodes report installer/nameplate ratings, not per-load live
         * readings. Preserve LoadMeasurements for actual sensor data only;
         * the sole INA219 lives on Central's battery bus.
         */
        existingLoad->setElectricalRatings(LoadElectricalRatings{
            loadPacket.nominalVoltageVolts,
            loadPacket.nominalCurrentAmps
        });

        /*
         * A Node report with confirmedRelayStateValid == 0 means the
         * reporting Load has no trustworthy relay read-back this cycle
         * (for example it has never yet had a successful confirmation).
         * setConfirmedRelayState() always marks its result valid, so
         * calling it unconditionally here would launder "unknown" into a
         * fabricated confirmed OFF on every such report. Only apply it
         * when the wire report actually vouches for the value; otherwise
         * leave whatever confirmed state/validity this Load already held
         * untouched, exactly like a failed relay command does.
         */
        if (loadPacket.confirmedRelayStateValid != 0U) {
            existingLoad->setConfirmedRelayState(loadPacket.confirmedRelayState != 0U);
        }
        existingLoad->setHealth(static_cast<LoadHealth>(loadPacket.availability));

        bool branchConfigurationFound = false;
        for (BranchConfiguration& branch : planningNode->branchConfigurations) {
            if (branch.relayPin == loadPacket.relayPin) {
                branch.maximumCurrentAmps = loadPacket.branchMaximumCurrentAmps;
                branchConfigurationFound = true;
                break;
            }
        }

        if (!branchConfigurationFound) {
            planningNode->branchConfigurations.push_back(BranchConfiguration{
                loadPacket.relayPin,
                loadPacket.branchMaximumCurrentAmps
            });
        }
    }

    /*
     * Prune a Load this Node no longer reports (removed on that Node, or
     * a freshly (re)commissioned Node now legitimately reporting fewer
     * Loads than before). Only safe for a complete, single-page report;
     * the check above already rejected any unsupported page, so pruning
     * can't delete Loads because a future partial report arrived.
     */
    if (packet.pageIndex == 0U && packet.totalPages == 1U) {
        std::vector<std::uint8_t> relayPinsStillReported;
        for (std::size_t i = 0; i < packet.numberOfLoads && i < MAX_LOADS_PER_NODE_PACKET; ++i) {
            if (isValidLoadReport(packet.loads[i])) {
                relayPinsStillReported.push_back(packet.loads[i].relayPin);
            }
        }

        std::vector<std::uint8_t> relayPinsToRemove;
        for (std::size_t i = 0U; i < planningNode->node.getNumberOfLoads(); ++i) {
            const Load *existing = planningNode->node.getLoad(i);
            if (existing == nullptr) {
                continue;
            }

            const std::uint8_t relayPin = existing->getRelayPin();
            const bool stillReported = std::find(relayPinsStillReported.begin(), relayPinsStillReported.end(),
                                                  relayPin) != relayPinsStillReported.end();
            if (!stillReported) {
                relayPinsToRemove.push_back(relayPin);
            }
        }

        for (std::uint8_t relayPin : relayPinsToRemove) {
            planningNode->node.removeLoadByRelayPin(relayPin);
        }
    }
}


std::size_t CentralNodeRegistry::getNumberOfNodes() const
{
    return planningNodes_.size();
}


const CentralNodeRegistry::PlanningNode* CentralNodeRegistry::getNode(std::size_t nodeIndex) const
{
    if (nodeIndex >= planningNodes_.size()) {
        return nullptr;
    }

    return &planningNodes_[nodeIndex];
}


const CentralNodeRegistry::PlanningNode* CentralNodeRegistry::findNodeByMacAddress(
    const MacAddress& macAddress) const
{
    for (const PlanningNode& planningNode : planningNodes_) {
        if (planningNode.node.getMacAddress() == macAddress) {
            return &planningNode;
        }
    }

    return nullptr;
}


CentralNodeRegistry::PlanningNode* CentralNodeRegistry::findMutablePlanningNode(
    const MacAddress& macAddress)
{
    for (PlanningNode& planningNode : planningNodes_) {
        if (planningNode.node.getMacAddress() == macAddress) {
            return &planningNode;
        }
    }

    return nullptr;
}


Load* CentralNodeRegistry::findMutableLoad(const MacAddress& macAddress, std::uint8_t relayPin)
{
    PlanningNode* planningNode = findMutablePlanningNode(macAddress);
    if (planningNode == nullptr) {
        return nullptr;
    }

    return planningNode->node.getLoadByRelayPin(relayPin);
}


bool CentralNodeRegistry::findBranchMaximumCurrentAmps(
    const MacAddress& macAddress,
    std::uint8_t relayPin,
    float& maximumCurrentAmps) const
{
    const PlanningNode* planningNode = findNodeByMacAddress(macAddress);
    if (planningNode == nullptr) {
        return false;
    }

    for (const BranchConfiguration& branch : planningNode->branchConfigurations) {
        if (branch.relayPin == relayPin) {
            maximumCurrentAmps = branch.maximumCurrentAmps;
            return true;
        }
    }

    return false;
}


bool CentralNodeRegistry::setLocalCentralBranchMaximumCurrentAmps(
    std::uint8_t relayPin,
    float maximumCurrentAmps)
{
    if (!std::isfinite(maximumCurrentAmps) || maximumCurrentAmps <= 0.0F) {
#ifdef ESP_PLATFORM
        ESP_LOGW(TAG, "Rejected local central Branch configuration: pin=%u maximumCurrentAmps=%.3f",
                 static_cast<unsigned int>(relayPin), maximumCurrentAmps);
#endif
        return false;
    }

    PlanningNode* localCentral = nullptr;
    for (PlanningNode& planningNode : planningNodes_) {
        if (planningNode.isCentralNode) {
            localCentral = &planningNode;
            break;
        }
    }

    if (localCentral == nullptr) {
#ifdef ESP_PLATFORM
        ESP_LOGW(TAG, "Cannot configure local central Branch pin=%u before addLocalCentralNode()",
                 static_cast<unsigned int>(relayPin));
#endif
        return false;
    }

    for (BranchConfiguration& branch : localCentral->branchConfigurations) {
        if (branch.relayPin == relayPin) {
            branch.maximumCurrentAmps = maximumCurrentAmps;
            return true;
        }
    }

    localCentral->branchConfigurations.push_back(BranchConfiguration{relayPin, maximumCurrentAmps});
    return true;
}


} // namespace kilowatts
