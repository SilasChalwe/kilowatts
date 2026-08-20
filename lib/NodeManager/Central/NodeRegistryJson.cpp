#include "NodeRegistryJson.h"

#include <cstdio>

namespace kilowatts {

namespace {

void appendString(std::string& out, const char* value)
{
    out.push_back('"');
    if (value != nullptr) {
        for (const char* c = value; *c != '\0'; ++c) {
            if (*c == '"' || *c == '\\') {
                out.push_back('\\');
            }
            out.push_back(*c);
        }
    }
    out.push_back('"');
}

bool pinIsUsed(const CentralNodeRegistry::PlanningNode* node, std::uint8_t pin)
{
    if (node == nullptr) {
        return false;
    }
    for (std::size_t i = 0U; i < node->node.getNumberOfLoads(); ++i) {
        const Load* load = node->node.getLoad(i);
        if (load != nullptr && load->getRelayPin() == pin) {
            return true;
        }
    }
    return false;
}

void appendRelayPins(
    std::string& out,
    const NodeCommissioningRegistry::CommissioningRecord& record,
    const CentralNodeRegistry::PlanningNode* node)
{
    out += "\"usedRelayPins\":[";
    bool first = true;
    for (std::size_t i = 0U; i < record.relayCapabilityCount; ++i) {
        const std::uint8_t pin = record.relayPins[i];
        if (!pinIsUsed(node, pin)) {
            continue;
        }
        if (!first) out += ",";
        first = false;
        out += std::to_string(static_cast<unsigned int>(pin));
    }
    out += "],\"availableRelayPins\":[";
    first = true;
    for (std::size_t i = 0U; i < record.relayCapabilityCount; ++i) {
        const std::uint8_t pin = record.relayPins[i];
        if (pinIsUsed(node, pin)) {
            continue;
        }
        if (!first) out += ",";
        first = false;
        out += std::to_string(static_cast<unsigned int>(pin));
    }
    out += "]";
}

void appendDiagnostics(
    std::string& out,
    const NodeCommissioningRegistry::Diagnostics& d)
{
    out += "{\"freeHeapBytes\":" + std::to_string(d.freeHeapBytes);
    out += ",\"minFreeHeapBytes\":" + std::to_string(d.minFreeHeapBytes);
    out += ",\"flashSizeBytes\":" + std::to_string(d.flashSizeBytes);
    out += ",\"psramSizeBytes\":" + std::to_string(d.psramSizeBytes);
    out += ",\"siliconRevision\":" + std::to_string(static_cast<unsigned int>(d.siliconRevision));
    out += ",\"cpuCores\":" + std::to_string(static_cast<unsigned int>(d.cpuCores));
    out += ",\"cpuFrequencyMhz\":" + std::to_string(d.cpuFrequencyMhz);
    out += ",\"resetReason\":";
    appendString(out, d.resetReason);
    out += ",\"temperatureAvailable\":";
    out += d.temperatureAvailable ? "true" : "false";
    out += ",\"temperatureCelsius\":";
    if (d.temperatureAvailable) {
        char value[24]{};
        std::snprintf(value, sizeof(value), "%.2f", static_cast<double>(d.temperatureCelsius));
        out += value;
    } else {
        out += "null";
    }
    out += "}";
}

} // namespace

void NodeRegistryJson::appendMacAddressJson(std::string& out, const MacAddress& mac)
{
    char text[18]{};
    std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    appendString(out, text);
}

const char* NodeRegistryJson::syncStateText(NodeCommissioningRegistry::SyncState state)
{
    switch (state) {
        case NodeCommissioningRegistry::SyncState::SYNCED: return "SYNCED";
        case NodeCommissioningRegistry::SyncState::PENDING: return "PENDING";
        case NodeCommissioningRegistry::SyncState::FAILED: return "FAILED";
    }
    return "UNKNOWN";
}

std::string NodeRegistryJson::buildStateNodesJson(
    const NodeCommissioningRegistry& commissioningRegistry,
    const CentralNodeRegistry& centralNodeRegistry,
    std::uint32_t schemaVersion,
    std::uint32_t nowMilliseconds,
    std::uint32_t onlineTimeoutMilliseconds)
{
    std::string json = "{\"schemaVersion\":" + std::to_string(schemaVersion) + ",\"nodes\":[";
    bool first = true;

    for (std::size_t i = 0U; i < commissioningRegistry.getCount(); ++i) {
        const auto* record = commissioningRegistry.getRecord(i);
        if (record == nullptr) {
            continue;
        }
        if (!first) json += ",";
        first = false;

        const auto* planningNode = centralNodeRegistry.findNodeByMacAddress(record->macAddress);
        const bool isCentral = planningNode != nullptr && planningNode->isCentralNode;

        json += "{\"mac\":";
        appendMacAddressJson(json, record->macAddress);
        json += ",\"role\":\"";
        json += toText(record->role);
        json += "\",\"branchName\":";
        appendString(json, record->friendlyName);
        json += ",\"lifecycleState\":\"";
        json += toText(record->lifecycleState);
        json += "\",\"syncState\":\"";
        json += syncStateText(record->syncState);
        json += "\",\"firmwareVersion\":";
        appendString(json, record->firmwareVersion);
        json += ",\"chipModel\":";
        appendString(json, record->chipModel);
        json += ",\"loadCount\":" + std::to_string(planningNode != nullptr ? planningNode->node.getNumberOfLoads() : 0U) + ",";
        appendRelayPins(json, *record, planningNode);
        json += ",\"diagnostics\":";
        appendDiagnostics(json, record->diagnostics);

        if (planningNode == nullptr) {
            json += ",\"online\":null,\"hopCountToCentral\":null,\"nextHopMac\":null";
        } else if (isCentral) {
            json += ",\"online\":true,\"hopCountToCentral\":0,\"nextHopMac\":null";
        } else {
            const bool online =
                (nowMilliseconds - planningNode->lastSeenMilliseconds) <= onlineTimeoutMilliseconds;
            json += ",\"online\":";
            json += online ? "true" : "false";
            json += ",\"hopCountToCentral\":" + std::to_string(planningNode->hopCountToCentral);
            json += ",\"nextHopMac\":";
            appendMacAddressJson(json, planningNode->nextHopToCentralMacAddress);
        }

        json += "}";
    }

    json += "]}";
    return json;
}

std::string NodeRegistryJson::buildConfigNodesJson(
    const NodeCommissioningRegistry& commissioningRegistry,
    const CentralNodeRegistry& centralNodeRegistry,
    std::uint32_t schemaVersion)
{
    std::string json = "{\"schemaVersion\":" + std::to_string(schemaVersion) + ",\"nodes\":[";
    bool first = true;

    for (std::size_t i = 0U; i < commissioningRegistry.getCount(); ++i) {
        const auto* record = commissioningRegistry.getRecord(i);
        if (record == nullptr) {
            continue;
        }
        if (!first) json += ",";
        first = false;

        const auto* planningNode = centralNodeRegistry.findNodeByMacAddress(record->macAddress);

        json += "{\"mac\":";
        appendMacAddressJson(json, record->macAddress);
        json += ",\"role\":\"";
        json += toText(record->role);
        json += "\",\"branchName\":";
        appendString(json, record->friendlyName);
        json += ",\"lifecycleState\":\"";
        json += toText(record->lifecycleState);
        json += "\",\"loadCount\":" + std::to_string(planningNode != nullptr ? planningNode->node.getNumberOfLoads() : 0U) + ",";
        appendRelayPins(json, *record, planningNode);
        json += "}";
    }

    json += "]}";
    return json;
}

} // namespace kilowatts
