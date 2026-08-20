/**
 * @file SystemStateJson.cpp
 * @brief Implements JSON construction for the kilowatts/v1/state/system
 *        MQTT topic.
 *
 * Plain string formatting, no ESP-IDF dependency, no JSON parsing (only
 * emission, hand-formatted rather than through a JSON library, since the
 * schema is small and fixed) — always compiled and fully host-testable
 * (see test/SystemStateJson/).
 */

#include "SystemStateJson.h"

#include <cstdio>

namespace kilowatts {


namespace {

void appendEscapedJsonString(std::string& out, const char* value)
{
    out.push_back('"');

    if (value != nullptr) {
        for (const char* c = value; *c != '\0'; ++c) {
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
    }

    out.push_back('"');
}


void appendNumberField(std::string& out, const char* key, float value, bool trailingComma)
{
    char buffer[48] = {};
    std::snprintf(buffer, sizeof(buffer), "\"%s\":%.3f%s", key, static_cast<double>(value), trailingComma ? "," : "");
    out += buffer;
}


void appendBoolField(std::string& out, const char* key, bool value, bool trailingComma)
{
    char buffer[48] = {};
    std::snprintf(buffer, sizeof(buffer), "\"%s\":%s%s", key, value ? "true" : "false", trailingComma ? "," : "");
    out += buffer;
}


void appendStringField(std::string& out, const char* key, const char* value, bool trailingComma)
{
    out += "\"";
    out += key;
    out += "\":";
    appendEscapedJsonString(out, value);
    if (trailingComma) {
        out += ",";
    }
}


void appendIntegerField(std::string& out, const char* key, std::int64_t value, bool trailingComma)
{
    char buffer[48] = {};
    std::snprintf(buffer, sizeof(buffer), "\"%s\":%lld%s", key, static_cast<long long>(value), trailingComma ? "," : "");
    out += buffer;
}

} // namespace


std::string SystemStateJson::build(const SystemStateInputs& inputs, std::uint32_t schemaVersion)
{
    std::string json;
    json.reserve(768);
    json += "{";

    appendIntegerField(json, "schemaVersion", static_cast<std::int64_t>(schemaVersion), true);

    json += "\"battery\":{";
    appendBoolField(json, "sensorConfigured", inputs.batterySensorConfigured, true);
    appendNumberField(json, "voltageVolts", inputs.batteryVoltageVolts, true);
    appendNumberField(json, "currentAmps", inputs.batteryCurrentAmps, true);
    appendStringField(json, "measurementSource", inputs.batteryMeasurementSourceText, true);
    appendNumberField(json, "stateOfChargePercent", inputs.stateOfChargePercent, true);
    appendBoolField(json, "stateOfChargeValid", inputs.stateOfChargeValid, true);
    appendStringField(json, "stateOfChargeSource", inputs.stateOfChargeSourceText, false);
    json += "},";

    json += "\"power\":{";
    appendNumberField(json, "estimatedTotalLoadPowerWatts", inputs.estimatedTotalLoadPowerWatts, true);
    appendNumberField(json, "availablePowerWatts", inputs.availablePowerWatts, true);
    appendNumberField(json, "fixedOnRunningPowerWatts", inputs.fixedOnRunningPowerWatts, true);
    appendNumberField(json, "powerAvailableForAutoLoadsWatts", inputs.powerAvailableForAutoLoadsWatts, true);
    appendNumberField(json, "remainingPowerWatts", inputs.remainingPowerWatts, true);
    appendNumberField(json, "committedPowerWatts", inputs.committedPowerWatts, false);
    json += "},";

    json += "\"connectivity\":{";
    appendBoolField(json, "wifiConnected", inputs.wifiConnected, true);
    appendStringField(json, "wifiState", inputs.wifiStateText, true);
    appendBoolField(json, "mqttConnected", inputs.mqttConnected, false);
    json += "},";

    json += "\"time\":{";
    appendBoolField(json, "valid", inputs.currentTimeValid, true);
    appendStringField(json, "source", inputs.currentTimeSourceText, true);
    appendIntegerField(json, "lastOptimizationEpochSeconds", inputs.lastOptimizationEpochSeconds, false);
    json += "},";

    json += "\"diagnostics\":{";
    appendIntegerField(json, "faultCount", static_cast<std::int64_t>(inputs.faultCount), true);
    appendStringField(json, "faultSummary", inputs.faultSummaryText != nullptr ? inputs.faultSummaryText : "", false);
    json += "}";

    json += "}";
    return json;
}


} // namespace kilowatts
