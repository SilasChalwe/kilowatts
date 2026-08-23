#include "TopologyTree.h"

#include <cstdio>

namespace kilowatts {

namespace {

void appendString(std::string& out, const std::string& value)
{
    out.push_back('"');
    for (char c : value) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
}

const char* controlModeText(const Load& load)
{
    return load.isFixed() ? "FIXED" : "AUTO";
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

const char* TopologyTree::rejectionReasonText(std::uint8_t reason)
{
    switch (reason) {
        case 0U: return "NONE";
        case 1U: return "LOW_BATTERY";
        case 2U: return "POWER_BUDGET_EXCEEDED";
        case 3U: return "BATTERY_CURRENT_LIMIT";
        case 4U: return "MAIN_LIMIT_EXCEEDED";
        case 5U: return "BRANCH_LIMIT_EXCEEDED";
    }
    return "UNKNOWN";
}

void TopologyTree::appendLoadJson(
    std::string& out,
    const Load& load,
    const std::string& nodeName)
{
    const AutoSchedule schedule = load.getSchedule();

    out += "{\"name\":";
    appendString(out, load.getName());
    out += ",\"nodeName\":";
    appendString(out, nodeName);
    out += ",\"nodeMac\":";
    appendMacAddressJson(out, load.getMacAddress());
    out += ",\"relayPin\":" + std::to_string(static_cast<unsigned int>(load.getRelayPin()));
    out += ",\"controlMode\":\"";
    out += controlModeText(load);
    out += "\",\"mode\":\"";
    out += loadModeText(load.getMode());
    out += "\",\"manualControlAllowed\":";
    out += load.isFixed() ? "true" : "false";
    out += ",\"priority\":" + std::to_string(static_cast<unsigned int>(load.getPriority()));
    out += ",\"powerRatingWatts\":" +
        std::to_string(static_cast<double>(load.getPowerRatingWatts()));
    out += ",\"powerType\":\"";
    out += load.getPowerType() == LoadPowerType::AC ? "AC" : "DC";
    out += "\"";
    out += ",\"schedule\":{\"enabled\":";
    out += schedule.enabled ? "true" : "false";
    out += ",\"hour\":" + std::to_string(static_cast<unsigned int>(schedule.hour));
    out += ",\"minute\":" + std::to_string(static_cast<unsigned int>(schedule.minute));
    out += "},\"bestFirstRejectionReason\":\"";
    out += rejectionReasonText(load.getLastBestFirstRejectionReason());
    out += "\"}";
}

void TopologyTree::appendLoadsForNode(
    std::string& out,
    const CentralNodeRegistry::PlanningNode& planningNode)
{
    out += "\"loads\":[";
    bool first = true;
    for (std::size_t index = 0U; index < planningNode.node.getNumberOfLoads(); ++index) {
        const Load* load = planningNode.node.getLoad(index);
        if (load == nullptr) continue;
        if (!first) out += ",";
        first = false;
        appendLoadJson(out, *load, planningNode.nodeName);
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
        const auto* planningNode = registry.getNode(index);
        if (planningNode == nullptr || planningNode->isCentralNode ||
            planningNode->nextHopToCentralMacAddress != parentMac) {
            continue;
        }

        if (!first) out += ",";
        first = false;

        const bool online =
            (nowMilliseconds - planningNode->lastSeenMilliseconds) <= onlineTimeoutMilliseconds;

        out += "{\"type\":\"node\",\"nodeRole\":\"SMART\",\"name\":";
        appendString(out, planningNode->nodeName);
        out += ",\"mac\":";
        appendMacAddressJson(out, planningNode->node.getMacAddress());
        out += ",\"parentMac\":";
        appendMacAddressJson(out, planningNode->nextHopToCentralMacAddress);
        out += ",\"hopCountToCentral\":" + std::to_string(planningNode->hopCountToCentral);
        out += ",\"online\":";
        out += online ? "true" : "false";
        out += ",";
        appendDiagnosticsJson(out, commissioningRegistry, planningNode->node.getMacAddress());
        out += ",";
        appendLoadsForNode(out, *planningNode);
        out += ",";

        if (depthRemaining > 0U) {
            appendChildren(out, registry, commissioningRegistry, planningNode->node.getMacAddress(),
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
    if (central == nullptr) return json + "null}";

    json += "{\"type\":\"node\",\"nodeRole\":\"CENTRAL\",\"name\":";
    appendString(json, central->nodeName);
    json += ",\"mac\":";
    appendMacAddressJson(json, central->node.getMacAddress());
    json += ",\"online\":true,";
    appendDiagnosticsJson(json, commissioningRegistry, central->node.getMacAddress());
    json += ",";
    appendLoadsForNode(json, *central);
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
        const auto* planningNode = registry.getNode(nodeIndex);
        if (planningNode == nullptr) continue;

        for (std::size_t loadIndex = 0U; loadIndex < planningNode->node.getNumberOfLoads(); ++loadIndex) {
            const Load* load = planningNode->node.getLoad(loadIndex);
            if (load == nullptr) continue;
            if (!first) json += ",";
            first = false;
            appendLoadJson(json, *load, planningNode->nodeName);
        }
    }

    json += "]}";
    return json;
}

} // namespace kilowatts