#include "SystemStateJson.h"

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

void number(std::string& out, const char* key, float value, bool comma = true)
{
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "\"%s\":%.3f%s",
                  key, static_cast<double>(value), comma ? "," : "");
    out += buffer;
}

void boolean(std::string& out, const char* key, bool value, bool comma = true)
{
    out += "\"";
    out += key;
    out += "\":";
    out += value ? "true" : "false";
    if (comma) {
        out += ",";
    }
}

void text(std::string& out, const char* key, const char* value, bool comma = true)
{
    out += "\"";
    out += key;
    out += "\":";
    appendString(out, value);
    if (comma) {
        out += ",";
    }
}

void integer(std::string& out, const char* key, std::int64_t value, bool comma = true)
{
    out += "\"";
    out += key;
    out += "\":" + std::to_string(value);
    if (comma) {
        out += ",";
    }
}

} // namespace

std::string SystemStateJson::build(const SystemStateInputs& in, std::uint32_t schemaVersion)
{
    std::string json;
    json.reserve(900);
    json += "{\"schemaVersion\":" + std::to_string(schemaVersion) + ",";

    json += "\"battery\":{";
    boolean(json, "sensorConfigured", in.batterySensorConfigured);
    number(json, "voltageVolts", in.batteryVoltageVolts);
    number(json, "currentAmps", in.batteryCurrentAmps);
    number(json, "powerWatts", in.batteryPowerWatts);
    text(json, "measurementSource", in.batteryMeasurementSourceText);
    number(json, "stateOfChargePercent", in.stateOfChargePercent);
    boolean(json, "stateOfChargeValid", in.stateOfChargeValid);
    text(json, "stateOfChargeSource", in.stateOfChargeSourceText);
    number(json, "estimatedRuntimeHours", in.estimatedRuntimeHours);
    boolean(json, "runtimeEstimateValid", in.runtimeEstimateValid, false);
    json += "},";

    json += "\"powerFlow\":{";
    number(json, "estimatedCurrentlyOnPowerWatts", in.estimatedCurrentlyOnPowerWatts);
    number(json, "safeAvailablePowerWatts", in.safeAvailablePowerWatts);
    number(json, "fixedOnLoadPowerWatts", in.fixedOnLoadPowerWatts);
    number(json, "initialBestFirstPowerWatts", in.initialBestFirstPowerWatts);
    number(json, "selectedAutoLoadPowerWatts", in.selectedAutoLoadPowerWatts);
    number(json, "finalRemainingPowerWatts", in.finalRemainingPowerWatts, false);
    json += "},";

    json += "\"connectivity\":{";
    boolean(json, "wifiConnected", in.wifiConnected);
    text(json, "wifiState", in.wifiStateText);
    boolean(json, "mqttConnected", in.mqttConnected, false);
    json += "},";

    json += "\"time\":{";
    boolean(json, "valid", in.currentTimeValid);
    text(json, "source", in.currentTimeSourceText);
    integer(json, "lastOptimizationEpochSeconds", in.lastOptimizationEpochSeconds, false);
    json += "},";

    json += "\"diagnostics\":{";
    integer(json, "faultCount", static_cast<std::int64_t>(in.faultCount));
    text(json, "faultSummary", in.faultSummaryText != nullptr ? in.faultSummaryText : "", false);
    json += "}}";
    return json;
}

} // namespace kilowatts
