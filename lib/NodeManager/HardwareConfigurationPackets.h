/**
 * @file HardwareConfigurationPackets.h
 * @brief ESP-NOW packets for adding and removing loads on Smart Nodes.
 */
#ifndef KILOWATTS_HARDWARE_CONFIGURATION_PACKETS_H
#define KILOWATTS_HARDWARE_CONFIGURATION_PACKETS_H

#include <cstdint>
#include <type_traits>

namespace kilowatts {

enum class HardwareConfigurationFailureReason : std::uint8_t {
    NONE = 0U,
    NODE_NOT_COMMISSIONED = 1U,
    UNSUPPORTED_RELAY_PIN = 2U,
    DUPLICATE_RELAY_PIN = 3U,
    INVALID_ELECTRICAL_RATING = 4U,
    INVALID_CONFIGURATION = 5U,
    HARDWARE_INITIALIZATION_FAILED = 6U,
    PERSISTENCE_FAILED = 7U,
    CAPACITY_REACHED = 8U
};

struct ConfigureLoadCommandPacket {
    std::uint32_t commandId;
    char loadName[16];
    std::uint8_t relayPin;
    std::uint8_t relayActiveHigh;
    std::uint8_t mode;
    std::uint16_t priority;
    float nominalVoltageVolts;
    float nominalCurrentAmps;
    float startupWatts;
    std::uint8_t scheduleEnabled;
    std::uint8_t scheduleHour;
    std::uint8_t scheduleMinute;
};

struct ConfigureLoadAcknowledgementPacket {
    std::uint32_t commandId;
    std::uint8_t relayPin;
    std::uint8_t success;
    std::uint8_t failureReason;
};

struct RemoveLoadCommandPacket {
    std::uint32_t commandId;
    std::uint8_t relayPin;
};

struct RemoveLoadAcknowledgementPacket {
    std::uint32_t commandId;
    std::uint8_t relayPin;
    std::uint8_t success;
    std::uint8_t failureReason;
};

static_assert(std::is_trivially_copyable<ConfigureLoadCommandPacket>::value,
              "ConfigureLoadCommandPacket must be safe for ESP-NOW");
static_assert(std::is_trivially_copyable<ConfigureLoadAcknowledgementPacket>::value,
              "ConfigureLoadAcknowledgementPacket must be safe for ESP-NOW");
static_assert(std::is_trivially_copyable<RemoveLoadCommandPacket>::value,
              "RemoveLoadCommandPacket must be safe for ESP-NOW");
static_assert(std::is_trivially_copyable<RemoveLoadAcknowledgementPacket>::value,
              "RemoveLoadAcknowledgementPacket must be safe for ESP-NOW");

} // namespace kilowatts

#endif // KILOWATTS_HARDWARE_CONFIGURATION_PACKETS_H
