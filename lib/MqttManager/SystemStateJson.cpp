#include "SystemStateJson.h"

#include <cmath>
#include <cstdio>

namespace kilowatts {
namespace {
void appendString(std::string& out, const char* value)
{
    out.push_back('"');
    if (value != nullptr) {
        for (const char* c = value; *c != '\0'; ++c) {
            if (*c == '"' || *c == '\\') out.push_back('\\');
            out.push_back(*c);
        }
    }
    out.push_back('"');
}

void number(std::string& out, const char* key, float value, bool comma = true)
{
    if (!std::isfinite(value)) {
        char buffer[64]{};
        std::snprintf(buffer, sizeof(buffer), "\"%s\":null%s", key, comma ? "," : "");
        out += buffer;
        return;
    }
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "\"%s\":%.3f%s", key, static_cast<double>(value), comma ? "," : "");
    out += buffer;
}

void boolean(std::string& out, const char* key, bool value, bool comma = true)
{
    out += "\""; out += key; out += "\":"; out += value ? "true" : "false";
    if (comma) out += ",";
}

void text(std::string& out, const char* key, const char* value, bool comma = true)
{
    out += "\""; out += key; out += "\":"; appendString(out, value);
    if (comma) out += ",";
}

void integer(std::string& out, const char* key, std::int64_t value, bool comma = true)
{
    out += "\""; out += key; out += "\":" + std::to_string(value);
    if (comma) out += ",";
}
} // namespace

std::string SystemStateJson::build(const SystemStateInputs& in, std::uint32_t schemaVersion)
{
    std::string json;
    json.reserve(1200);
    json += "{\"schemaVersion\":" + std::to_string(schemaVersion) + ",";

    json += "\"battery\":{";
    boolean(json, "sensorConfigured", in.batterySensorConfigured);
    number(json, "nominalVoltageVolts", in.batteryNominalVoltageVolts);
    number(json, "capacityAmpHours", in.batteryCapacityAmpHours);
    number(json, "ratedEnergyWattHours", in.batteryRatedEnergyWattHours);
    number(json, "storedEnergyWattHours", in.batteryStoredEnergyWattHours);
    number(json, "usableEnergyWattHours", in.batteryUsableEnergyWattHours);
    number(json, "voltageVolts", in.batteryVoltageVolts);
    number(json, "currentAmps", in.batteryCurrentAmps);
    number(json, "P_measured", in.P_measured);
    text(json, "measurementSource", in.batteryMeasurementSourceText);
    number(json, "stateOfChargePercent", in.stateOfChargePercent);
    boolean(json, "stateOfChargeValid", in.stateOfChargeValid);
    text(json, "stateOfChargeSource", in.stateOfChargeSourceText);
    boolean(json, "batteryReserveReached", in.batteryReserveReached);
    boolean(json, "requiredRuntimeConfigured", in.requiredRuntimeConfigured);
    number(json, "requiredRuntimeHours", in.requiredRuntimeHours);
    number(json, "remainingRuntimeHours", in.remainingRuntimeHours);
    number(json, "estimatedRuntimeHours", in.estimatedRuntimeHours);
    boolean(json, "runtimeEstimateValid", in.runtimeEstimateValid);
    number(json, "P_runtime", in.P_runtime);
    boolean(json, "requiredRuntimeAchievable", in.requiredRuntimeAchievable, false);
    json += "},";

    json += "\"powerFlow\":{";
    number(json, "P_budget", in.P_budget);
    number(json, "P_reserve", in.P_reserve);
    number(json, "P_usable", in.P_usable);
    number(json, "P_fixed", in.P_fixed);
    number(json, "P_auto_available", in.P_auto_available);
    number(json, "P_auto", in.P_auto);
    number(json, "P_remaining", in.P_remaining, false);
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
    integer(json, "pinCommandErrorCount", static_cast<std::int64_t>(in.pinCommandErrorCount), false);
    json += "}}";
    return json;
}

} // namespace kilowatts
