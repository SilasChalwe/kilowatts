/**
 * @file NodeRegistryJson.cpp
 * @brief Implements JSON construction for the kilowatts/v1/state/nodes and
 *        kilowatts/v1/config/nodes MQTT topics.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 14 August 2026
 */

#include "NodeRegistryJson.h"

#include <cstdio>

namespace kilowatts {


namespace {

void appendEscapedJsonString(std::string& out, const char* value)
{
    out.push_back('"');

    for (const char* c = value; c != nullptr && *c != '\0'; ++c) {
        switch (*c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(*c) < 0x20U) {
                    char escaped[8] = {};
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned int>(*c));
                    out += escaped;
                } else {
                    out.push_back(*c);
                }
                break;
        }
    }

    out.push_back('"');
}

void appendRelayCapabilitiesJson(std::string& out, const NodeCommissioningRegistry::CommissioningRecord& record)
{
    out += "[";
    for (std::size_t index = 0U; index < record.relayCapabilityCount; ++index) {
        if (index > 0U) {
            out += ",";
        }
        out += std::to_string(static_cast<unsigned int>(record.relayPins[index]));
    }
    out += "]";
}

} // namespace


void NodeRegistryJson::appendMacAddressJson(std::string& out, const MacAddress& macAddress)
{
    char text[18] = {};
    std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
                  macAddress[0], macAddress[1], macAddress[2], macAddress[3], macAddress[4], macAddress[5]);
    out += "\"";
    out += text;
    out += "\"";
}


const char* NodeRegistryJson::syncStateText(NodeCommissioningRegistry::SyncState syncState)
{
    switch (syncState) {
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
    std::string json;
    json.reserve(512);

    json += "{\"schemaVersion\":" + std::to_string(schemaVersion) + ",\"nodes\":[";

    for (std::size_t i = 0U; i < commissioningRegistry.getCount(); ++i) {
        const NodeCommissioningRegistry::CommissioningRecord* record = commissioningRegistry.getRecord(i);
        if (record == nullptr) {
            continue;
        }

        if (i > 0U) {
            json += ",";
        }

        json += "{\"mac\":";
        appendMacAddressJson(json, record->macAddress);
        json += ",\"role\":\"";
        json += toText(record->role);
        json += "\",\"name\":";
        appendEscapedJsonString(json, record->friendlyName);
        json += ",\"lifecycleState\":\"";
        json += toText(record->lifecycleState);
        json += "\",\"syncState\":\"";
        json += syncStateText(record->syncState);
        json += "\",\"firmwareVersion\":";
        appendEscapedJsonString(json, record->firmwareVersion);
        json += ",\"chipModel\":";
        appendEscapedJsonString(json, record->chipModel);
        json += ",\"relayCapabilities\":";
        appendRelayCapabilitiesJson(json, *record);

        const CentralNodeRegistry::PlanningNode* planningNode =
            centralNodeRegistry.findNodeByMacAddress(record->macAddress);

        if (planningNode == nullptr) {
            json += ",\"online\":null,\"hopCountToCentral\":null,\"nextHopMac\":null,\"lastSeenMilliseconds\":null";
        } else if (planningNode->isCentralNode) {
            json += ",\"online\":true,\"hopCountToCentral\":0,\"nextHopMac\":null";
            json += ",\"lastSeenMilliseconds\":" + std::to_string(planningNode->lastSeenMilliseconds);
        } else {
            const std::uint32_t elapsedMilliseconds = nowMilliseconds - planningNode->lastSeenMilliseconds;
            const bool online = elapsedMilliseconds <= onlineTimeoutMilliseconds;

            json += ",\"online\":";
            json += online ? "true" : "false";
            json += ",\"hopCountToCentral\":" + std::to_string(static_cast<unsigned int>(planningNode->hopCountToCentral));
            json += ",\"nextHopMac\":";
            appendMacAddressJson(json, planningNode->nextHopToCentralMacAddress);
            json += ",\"lastSeenMilliseconds\":" + std::to_string(planningNode->lastSeenMilliseconds);
        }

        json += "}";
    }

    json += "]}";
    return json;
}


std::string NodeRegistryJson::buildConfigNodesJson(
    const NodeCommissioningRegistry& commissioningRegistry,
    std::uint32_t schemaVersion)
{
    std::string json;
    json.reserve(256);

    json += "{\"schemaVersion\":" + std::to_string(schemaVersion) + ",\"nodes\":[";

    for (std::size_t i = 0U; i < commissioningRegistry.getCount(); ++i) {
        const NodeCommissioningRegistry::CommissioningRecord* record = commissioningRegistry.getRecord(i);
        if (record == nullptr) {
            continue;
        }

        if (i > 0U) {
            json += ",";
        }

        json += "{\"mac\":";
        appendMacAddressJson(json, record->macAddress);
        json += ",\"role\":\"";
        json += toText(record->role);
        json += "\",\"name\":";
        appendEscapedJsonString(json, record->friendlyName);
        json += ",\"lifecycleState\":\"";
        json += toText(record->lifecycleState);
        json += "\",\"syncState\":\"";
        json += syncStateText(record->syncState);
        json += "\",\"firmwareVersion\":";
        appendEscapedJsonString(json, record->firmwareVersion);
        json += ",\"chipModel\":";
        appendEscapedJsonString(json, record->chipModel);
        json += ",\"relayCapabilities\":";
        appendRelayCapabilitiesJson(json, *record);
        json += "}";
    }

    json += "]}";
    return json;
}


} // namespace kilowatts
