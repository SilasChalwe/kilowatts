#include "TopologyTree.h"

#include "BestFirstSearch.h"

#include <cstdio>

namespace kilowatts {

namespace {

void appendString(std::string& out, const std::string& value)
{
    out.push_back('"');
    for (char c : value) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
}

} // namespace

void TopologyTree::appendMacAddressJson(std::string& out, const Load::MacAddress& mac)
{
    char text[18]{};
    std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    appendString(out, text);
}

const char* TopologyTree::loadModeText(LoadMode::Value mode)
{
    switch (mode) {
        case LoadMode::Value::FIXED_OFF: return "FIXED_OFF";
        case LoadMode::Value::FIXED_ON: return "FIXED_ON";
        case LoadMode::Value::AUTO_OFF: return "AUTO_OFF";
        case LoadMode::Value::AUTO_ON: return "AUTO_ON";
    }
    return "UNKNOWN";
}

const char* TopologyTree::loadHealthText(LoadHealth health)
{
    switch (health) {
        case LoadHealth::AVAILABLE: return "AVAILABLE";
        case LoadHealth::FAULTED: return "FAULTED";
        case LoadHealth::UNAVAILABLE: return "UNAVAILABLE";
    }
    return "UNKNOWN";
}

const char* TopologyTree::rejectionReasonText(std::uint8_t reason)
{
    switch (reason) {
        case BestFirstSearch::NONE: return "NONE";
        case BestFirstSearch::LOW_BATTERY: return "LOW_BATTERY";
        case BestFirstSearch::POWER_LIMIT_EXCEEDED: return "POWER_LIMIT_EXCEEDED";
        case BestFirstSearch::BATTERY_CURRENT_LIMIT: return "BATTERY_CURRENT_LIMIT";
        case BestFirstSearch::MAIN_LIMIT_EXCEEDED: return "MAIN_LIMIT_EXCEEDED";
    }
    return "UNKNOWN";
}

void TopologyTree::appendLoadJson(
    std::string& out,
    const Load& load,
    const std::string& branchName)
{
    const LoadPower power = load.getPower();
    const LoadElectricalRatings ratings = load.getElectricalRatings();
    const AutoSchedule schedule = load.getSchedule();

    out += "{\"name\":";
    appendString(out, load.getName());
    out += ",\"branchName\":";
    appendString(out, branchName);
    out += ",\"nodeMac\":";
    appendMacAddressJson(out, load.getMacAddress());
    out += ",\"relayPin\":" + std::to_string(static_cast<unsigned int>(load.getRelayPin()));
    out += ",\"mode\":\"";
    out += loadModeText(load.getMode());
    out += "\",\"targetOn\":";
    out += load.getTargetRelayState() ? "true" : "false";
    out += ",\"confirmedOn\":";
    out += load.getConfirmedRelayState() ? "true" : "false";
    out += ",\"confirmedStateValid\":";
    out += load.isConfirmedRelayStateValid() ? "true" : "false";
    out += ",\"health\":\"";
    out += loadHealthText(load.getHealth());
    out += "\",\"priority\":" + std::to_string(static_cast<unsigned int>(load.getPriority()));
    out += ",\"runningWatts\":" + std::to_string(static_cast<double>(power.runningWatts));
    out += ",\"startupWatts\":" + std::to_string(static_cast<double>(power.startupWatts));
    out += ",\"nominalVoltageVolts\":" + std::to_string(static_cast<double>(ratings.nominalVoltageVolts));
    out += ",\"nominalCurrentAmps\":" + std::to_string(static_cast<double>(ratings.nominalCurrentAmps));
    out += ",\"schedule\":{\"enabled\":";
    out += schedule.enabled ? "true" : "false";
    out += ",\"hour\":" + std::to_string(static_cast<unsigned int>(schedule.hour));
    out += ",\"minute\":" + std::to_string(static_cast<unsigned int>(schedule.minute));
    out += "},\"bestFirstRejectionReason\":\"";
    out += rejectionReasonText(load.getLastBestFirstRejectionReason());
    out += "\"}";
}

void TopologyTree::appendLoadsForBranch(
    std::string& out,
    const CentralNodeRegistry::PlanningNode& branch)
{
    out += "\"loads\":[";
    bool first = true;
    for (std::size_t index = 0U; index < branch.node.getNumberOfLoads(); ++index) {
        const Load* load = branch.node.getLoad(index);
        if (load == nullptr) {
            continue;
        }
        if (!first) {
            out += ",";
        }
        first = false;
        appendLoadJson(out, *load, branch.nodeName);
    }
    out += "]";
}

void TopologyTree::appendDiagnosticsJson(
    std::string& out,
    const NodeCommissioningRegistry& commissioningRegistry,
    const Load::MacAddress& mac)
{
    const NodeCommissioningRegistry::CommissioningRecord* record = commissioningRegistry.findByMac(mac);
    out += "\"diagnostics\":";
    if (record == nullptr) {
        out += "null";
        return;
    }

    const auto& d = record->diagnostics;
    out += "{\"firmwareVersion\":";
    appendString(out, record->firmwareVersion);
    out += ",\"chipModel\":";
    appendString(out, record->chipModel);
    out += ",\"freeHeapBytes\":" + std::to_string(d.freeHeapBytes);
    out += ",\"minFreeHeapBytes\":" + std::to_string(d.minFreeHeapBytes);
    out += ",\"flashSizeBytes\":" + std::to_string(d.flashSizeBytes);
    out += ",\"psramSizeBytes\":" + std::to_string(d.psramSizeBytes);
    out += ",\"cpuCores\":" + std::to_string(static_cast<unsigned int>(d.cpuCores));
    out += ",\"resetReason\":";
    appendString(out, d.resetReason);
    out += "}";
}

void TopologyTree::appendChildren(
    std::string& out,
    const CentralNodeRegistry& registry,
    const NodeCommissioningRegistry& commissioningRegistry,
    const Load::MacAddress& parentMac,
    std::uint32_t nowMilliseconds,
    std::uint32_t onlineTimeoutMilliseconds,
    std::size_t depthRemaining)
{
    out += "\"children\":[";
    bool first = true;

    for (std::size_t index = 0U; index < registry.getNumberOfNodes(); ++index) {
        const auto* branch = registry.getNode(index);
        if (branch == nullptr || branch->isCentralNode ||
            branch->nextHopToCentralMacAddress != parentMac) {
            continue;
        }

        if (!first) {
            out += ",";
        }
        first = false;

        const bool online =
            (nowMilliseconds - branch->lastSeenMilliseconds) <= onlineTimeoutMilliseconds;

        out += "{\"type\":\"branch\",\"nodeRole\":\"SMART\",\"name\":";
        appendString(out, branch->nodeName);
        out += ",\"mac\":";
        appendMacAddressJson(out, branch->node.getMacAddress());
        out += ",\"parentMac\":";
        appendMacAddressJson(out, branch->nextHopToCentralMacAddress);
        out += ",\"hopCountToCentral\":" + std::to_string(branch->hopCountToCentral);
        out += ",\"online\":";
        out += online ? "true" : "false";
        out += ",";
        appendDiagnosticsJson(out, commissioningRegistry, branch->node.getMacAddress());
        out += ",";
        appendLoadsForBranch(out, *branch);
        out += ",";

        if (depthRemaining > 0U) {
            appendChildren(out, registry, commissioningRegistry, branch->node.getMacAddress(),
                           nowMilliseconds, onlineTimeoutMilliseconds, depthRemaining - 1U);
        } else {
            out += "\"children\":[]";
        }
        out += "}";
    }

    out += "]";
}

std::string TopologyTree::buildTreeJson(
    const CentralNodeRegistry& registry,
    const NodeCommissioningRegistry& commissioningRegistry,
    std::uint32_t schemaVersion,
    std::uint32_t nowMilliseconds,
    std::uint32_t onlineTimeoutMilliseconds)
{
    const CentralNodeRegistry::PlanningNode* central = nullptr;
    for (std::size_t index = 0U; index < registry.getNumberOfNodes(); ++index) {
        const auto* candidate = registry.getNode(index);
        if (candidate != nullptr && candidate->isCentralNode) {
            central = candidate;
            break;
        }
    }

    std::string json = "{\"schemaVersion\":" + std::to_string(schemaVersion) + ",\"central\":";
    if (central == nullptr) {
        return json + "null}";
    }

    json += "{\"type\":\"branch\",\"nodeRole\":\"CENTRAL\",\"name\":";
    appendString(json, central->nodeName);
    json += ",\"mac\":";
    appendMacAddressJson(json, central->node.getMacAddress());
    json += ",\"online\":true,";
    appendDiagnosticsJson(json, commissioningRegistry, central->node.getMacAddress());
    json += ",";
    appendLoadsForBranch(json, *central);
    json += ",";
    appendChildren(json, registry, commissioningRegistry, central->node.getMacAddress(),
                   nowMilliseconds, onlineTimeoutMilliseconds, registry.getNumberOfNodes());
    json += "}}";
    return json;
}

std::string TopologyTree::buildLoadsJson(
    const CentralNodeRegistry& registry,
    std::uint32_t schemaVersion)
{
    std::string json = "{\"schemaVersion\":" + std::to_string(schemaVersion) + ",\"loads\":[";
    bool first = true;

    for (std::size_t nodeIndex = 0U; nodeIndex < registry.getNumberOfNodes(); ++nodeIndex) {
        const auto* branch = registry.getNode(nodeIndex);
        if (branch == nullptr) {
            continue;
        }
        for (std::size_t loadIndex = 0U; loadIndex < branch->node.getNumberOfLoads(); ++loadIndex) {
            const Load* load = branch->node.getLoad(loadIndex);
            if (load == nullptr) {
                continue;
            }
            if (!first) {
                json += ",";
            }
            first = false;
            appendLoadJson(json, *load, branch->nodeName);
        }
    }

    json += "]}";
    return json;
}

} // namespace kilowatts
