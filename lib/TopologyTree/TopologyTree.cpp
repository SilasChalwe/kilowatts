/**
 * @file TopologyTree.cpp
 * @brief Implements JSON construction for the kilowatts/v1/state/tree and
 *        kilowatts/v1/state/loads MQTT topics.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
 */

#include "TopologyTree.h"

#include "BestFirstSearch.h"

#include <cstdio>

namespace kilowatts {


namespace {

void appendEscapedJsonString(std::string& out, const std::string& value)
{
    out.push_back('"');

    for (char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20U) {
                    char escaped[8] = {};
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned int>(c));
                    out += escaped;
                } else {
                    out.push_back(c);
                }
                break;
        }
    }

    out.push_back('"');
}

} // namespace


void TopologyTree::appendMacAddressJson(std::string& out, const Load::MacAddress& macAddress)
{
    char text[18] = {};
    std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
                  macAddress[0], macAddress[1], macAddress[2], macAddress[3], macAddress[4], macAddress[5]);
    out += "\"";
    out += text;
    out += "\"";
}


const char* TopologyTree::loadModeText(LoadMode::Value mode)
{
    switch (mode) {
        case LoadMode::Value::FIXED_OFF: return "FIXED_OFF";
        case LoadMode::Value::FIXED_ON: return "FIXED_ON";
        case LoadMode::Value::AUTO_OFF: return "AUTO_OFF";
        case LoadMode::Value::AUTO_ON: return "AUTO_ON";
        default: return "UNKNOWN";
    }
}


const char* TopologyTree::loadHealthText(LoadHealth health)
{
    switch (health) {
        case LoadHealth::AVAILABLE: return "AVAILABLE";
        case LoadHealth::FAULTED: return "FAULTED";
        case LoadHealth::UNAVAILABLE: return "UNAVAILABLE";
        default: return "UNKNOWN";
    }
}


const char* TopologyTree::rejectionReasonText(std::uint8_t reason)
{
    switch (reason) {
        case BestFirstSearch::NONE: return "NONE";
        case BestFirstSearch::LOW_BATTERY: return "LOW_BATTERY";
        case BestFirstSearch::POWER_BUDGET_EXCEEDED: return "POWER_BUDGET_EXCEEDED";
        case BestFirstSearch::BATTERY_CURRENT_LIMIT: return "BATTERY_CURRENT_LIMIT";
        case BestFirstSearch::MAIN_LIMIT_EXCEEDED: return "MAIN_LIMIT_EXCEEDED";
        case BestFirstSearch::BRANCH_LIMIT_EXCEEDED: return "BRANCH_LIMIT_EXCEEDED";
        default: return "UNKNOWN";
    }
}


void TopologyTree::appendLoadJson(std::string& out, const Load& load)
{
    const LoadPower power = load.getPower();
    const LoadElectricalRatings ratings = load.getElectricalRatings();
    const AutoSchedule schedule = load.getSchedule();

    out += "{";
    out += "\"relayPin\":";
    out += std::to_string(static_cast<unsigned int>(load.getRelayPin()));
    out += ",\"name\":";
    appendEscapedJsonString(out, load.getName());
    out += ",\"nodeMac\":";
    appendMacAddressJson(out, load.getMacAddress());
    out += ",\"mode\":\"";
    out += loadModeText(load.getMode());
    out += "\",\"targetOn\":";
    out += load.getTargetRelayState() ? "true" : "false";
    out += ",\"confirmedOn\":";
    out += load.getConfirmedRelayState() ? "true" : "false";
    out += ",\"confirmedStateValid\":";
    out += load.isConfirmedRelayStateValid() ? "true" : "false";
    out += ",\"priority\":";
    out += std::to_string(static_cast<unsigned int>(load.getPriority()));
    out += ",\"startupWatts\":" + std::to_string(static_cast<double>(power.startupWatts));
    out += ",\"nominalVoltageVolts\":" + std::to_string(static_cast<double>(ratings.nominalVoltageVolts));
    out += ",\"nominalCurrentAmps\":" + std::to_string(static_cast<double>(ratings.nominalCurrentAmps));
    out += ",\"nominalPowerWatts\":" + std::to_string(static_cast<double>(power.runningWatts));
    out += ",\"perLoadMeasurementAvailable\":false";
    out += ",\"scheduleEnabled\":";
    out += schedule.enabled ? "true" : "false";
    out += ",\"scheduleHour\":" + std::to_string(static_cast<unsigned int>(schedule.hour));
    out += ",\"scheduleMinute\":" + std::to_string(static_cast<unsigned int>(schedule.minute));
    out += ",\"health\":\"";
    out += loadHealthText(load.getHealth());
    out += "\",\"rejectionReason\":\"";
    out += rejectionReasonText(load.getLastBestFirstRejectionReason());
    out += "\"}";
}


void TopologyTree::appendBranchesForNode(std::string& out, const CentralNodeRegistry::PlanningNode& planningNode)
{
    out += "\"branches\":[";

    for (std::size_t i = 0U; i < planningNode.node.getNumberOfLoads(); ++i) {
        const Load* load = planningNode.node.getLoad(i);
        if (load == nullptr) {
            continue;
        }

        if (i > 0U) {
            out += ",";
        }

        float maximumCurrentAmps = 0.0F;
        bool hasBranchConfiguration = false;
        for (const CentralNodeRegistry::BranchConfiguration& branch : planningNode.branchConfigurations) {
            if (branch.relayPin == load->getRelayPin()) {
                maximumCurrentAmps = branch.maximumCurrentAmps;
                hasBranchConfiguration = true;
                break;
            }
        }

        out += "{\"type\":\"branch\",\"nodeMac\":";
        appendMacAddressJson(out, planningNode.node.getMacAddress());
        out += ",\"relayPin\":" + std::to_string(static_cast<unsigned int>(load->getRelayPin()));
        out += ",\"maximumCurrentAmps\":" + std::to_string(static_cast<double>(maximumCurrentAmps));
        out += ",\"maximumCurrentConfigured\":";
        out += hasBranchConfiguration ? "true" : "false";
        out += ",\"load\":";
        appendLoadJson(out, *load);
        out += "}";
    }

    out += "]";
}


void TopologyTree::appendNodeAndChildren(
    std::string& out,
    const CentralNodeRegistry& registry,
    const Load::MacAddress& nodeMacAddress,
    std::uint32_t nowMilliseconds,
    std::uint32_t onlineTimeoutMilliseconds,
    std::size_t remainingDepthGuard)
{
    out += "\"children\":[";

    bool firstChild = true;

    for (std::size_t i = 0U; i < registry.getNumberOfNodes(); ++i) {
        const CentralNodeRegistry::PlanningNode* planningNode = registry.getNode(i);
        if (planningNode == nullptr || planningNode->isCentralNode) {
            continue;
        }

        if (planningNode->nextHopToCentralMacAddress != nodeMacAddress) {
            continue;
        }

        if (!firstChild) {
            out += ",";
        }
        firstChild = false;

        const std::uint32_t elapsedMilliseconds = nowMilliseconds - planningNode->lastSeenMilliseconds;
        const bool online = elapsedMilliseconds <= onlineTimeoutMilliseconds;

        out += "{\"type\":\"smartNode\",\"name\":";
        appendEscapedJsonString(out, planningNode->nodeName);
        out += ",\"mac\":";
        appendMacAddressJson(out, planningNode->node.getMacAddress());
        out += ",\"parentMac\":";
        appendMacAddressJson(out, planningNode->nextHopToCentralMacAddress);
        out += ",\"hopCountToCentral\":" + std::to_string(static_cast<unsigned int>(planningNode->hopCountToCentral));
        out += ",\"online\":";
        out += online ? "true" : "false";
        out += ",";
        appendBranchesForNode(out, *planningNode);
        out += ",";

        /*
         * remainingDepthGuard bounds recursion the same way the pre-tree
         * printer in src/central/main.cpp historically did: a legitimate
         * chain can visit every known Node once, so reaching zero can
         * only mean a corrupted Next-Hop relationship (a routing loop),
         * never a real topology.
         */
        if (remainingDepthGuard == 0U) {
            out += "\"children\":[]";
        } else {
            appendNodeAndChildren(out, registry, planningNode->node.getMacAddress(),
                                   nowMilliseconds, onlineTimeoutMilliseconds, remainingDepthGuard - 1U);
        }

        out += "}";
    }

    out += "]";
}


std::string TopologyTree::buildTreeJson(
    const CentralNodeRegistry& registry,
    std::uint32_t schemaVersion,
    std::uint32_t nowMilliseconds,
    std::uint32_t onlineTimeoutMilliseconds)
{
    std::string json;
    json.reserve(1024);

    json += "{\"schemaVersion\":" + std::to_string(schemaVersion) + ",\"central\":";

    const CentralNodeRegistry::PlanningNode* central = nullptr;
    for (std::size_t i = 0U; i < registry.getNumberOfNodes(); ++i) {
        const CentralNodeRegistry::PlanningNode* candidate = registry.getNode(i);
        if (candidate != nullptr && candidate->isCentralNode) {
            central = candidate;
            break;
        }
    }

    if (central == nullptr) {
        json += "null}";
        return json;
    }

    json += "{\"type\":\"central\",\"name\":";
    appendEscapedJsonString(json, central->nodeName);
    json += ",\"mac\":";
    appendMacAddressJson(json, central->node.getMacAddress());
    json += ",\"online\":true,";
    appendBranchesForNode(json, *central);
    json += ",";

    appendNodeAndChildren(json, registry, central->node.getMacAddress(),
                           nowMilliseconds, onlineTimeoutMilliseconds, registry.getNumberOfNodes());

    json += "}}";
    return json;
}


std::string TopologyTree::buildLoadsJson(const CentralNodeRegistry& registry, std::uint32_t schemaVersion)
{
    std::string json;
    json.reserve(1024);

    json += "{\"schemaVersion\":" + std::to_string(schemaVersion) + ",\"loads\":[";

    bool firstLoad = true;

    for (std::size_t nodeIndex = 0U; nodeIndex < registry.getNumberOfNodes(); ++nodeIndex) {
        const CentralNodeRegistry::PlanningNode* planningNode = registry.getNode(nodeIndex);
        if (planningNode == nullptr) {
            continue;
        }

        for (std::size_t loadIndex = 0U; loadIndex < planningNode->node.getNumberOfLoads(); ++loadIndex) {
            const Load* load = planningNode->node.getLoad(loadIndex);
            if (load == nullptr) {
                continue;
            }

            if (!firstLoad) {
                json += ",";
            }
            firstLoad = false;

            appendLoadJson(json, *load);
        }
    }

    json += "]}";
    return json;
}


} // namespace kilowatts
