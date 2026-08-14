/**
 * @file CentralNodeRegistry.cpp
 * @brief Implements Central's planning-time view of every known Node.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 8 May 2026
 */

#include "CentralNodeRegistry.h"

#include "Load.h"

#include <algorithm>
#include <cmath>

/*
 * esp_log.h and every ESP_LOGx() call are only compiled on a real ESP32
 * build (ESP_PLATFORM is the macro ESP-IDF defines for every component
 * build), matching the pattern already used by BestFirstSearch.cpp and
 * INA219Monitor.cpp: the host build (run_cpp_test.sh, plain g++, no
 * ESP-IDF headers available) simply compiles without logging rather than
 * needing a fake stub header, and production logging on the real target
 * is completely unchanged.
 */
#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif

namespace kilowatts {


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

    planningNode->nodeName = packet.nodeName;
    planningNode->nextHopToCentralMacAddress = packet.upstreamNodeMacAddress;
    planningNode->hopCountToCentral = packet.hopCountToCentral;
    planningNode->lastSeenMilliseconds = nowMilliseconds;

    for (std::size_t i = 0; i < packet.numberOfLoads && i < MAX_LOADS_PER_NODE_PACKET; ++i) {
        const LoadReportPacket& loadPacket = packet.loads[i];

        /*
         * The same physical Load (this Node's MAC address + this relay
         * pin) is looked up so a repeated report updates it in place
         * instead of being rejected as a duplicate relay pin or adding a
         * second entry for the same Load.
         */
        Load *existingLoad = planningNode->node.getLoadByRelayPin(loadPacket.relayPin);

        if (existingLoad == nullptr) {
            const bool added = planningNode->node.addLoad(Load(
                Load::Id{packet.nodeMacAddress, loadPacket.relayPin},
                loadPacket.name,
                LoadPower{loadPacket.runningWatts, loadPacket.startupWatts},
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
            existingLoad->setPower({loadPacket.runningWatts, loadPacket.startupWatts});
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

        existingLoad->setMeasurements(LoadMeasurements{
            loadPacket.measuredVoltageVolts,
            loadPacket.measuredCurrentAmps,
            loadPacket.measuredPowerWatts
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

        /*
         * Branch configuration (I_branch,max) for this relay pin, updated
         * or inserted in place exactly like a Load above.
         */
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
     * Prune a Load this Node no longer reports (for example after a
     * Load/Branch was removed on that Node, or - the case this phase
     * makes routine - a freshly (re)commissioned Node now legitimately
     * reports fewer Loads, down to zero, than it ever did before). Only
     * safe when packet is a complete, single-page report: this project
     * does not yet implement multi-page reassembly (see
     * NodeReportPackets.h), so every real report today already satisfies
     * this, and pruning against an incomplete page would otherwise wrongly
     * delete Loads that simply live on a page not yet received.
     */
    if (packet.pageIndex == 0U && packet.totalPages == 1U) {
        std::vector<std::uint8_t> relayPinsStillReported;
        for (std::size_t i = 0; i < packet.numberOfLoads && i < MAX_LOADS_PER_NODE_PACKET; ++i) {
            relayPinsStillReported.push_back(packet.loads[i].relayPin);
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
