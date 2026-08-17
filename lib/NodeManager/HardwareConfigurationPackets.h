/**
 * @file HardwareConfigurationPackets.h
 * @brief Rare ESP-NOW messages used to commission a Smart Node's physical
 *        relay/load channel at runtime.
 *
 * Installation topology is never compiled into firmware. Central forwards
 * one validated CONFIGURE_LOAD MQTT command to the addressed Smart Node;
 * the Node validates it against its board-declared relay capabilities,
 * applies it locally and persists it before replying with this ACK. The
 * final cost-conscious design has one INA219 only at Central's battery bus;
 * Smart-node load values are installer-entered electrical ratings, not
 * individual live measurements.
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

/**
 * Wire-only representation of every fact needed to create one load channel
 * on the target Smart Node. nominalVoltageVolts and nominalCurrentAmps are
 * installation/nameplate values supplied by the installer; their product is
 * the conservative planned running power. They are not a sensor reading.
 * The enclosing ESP-NOW header already identifies that target, so this
 * packet intentionally contains no Node MAC address.
 */
struct ConfigureLoadCommandPacket {
    std::uint32_t commandId;
    char loadName[16];
    std::uint8_t relayPin;
    std::uint8_t relayActiveHigh;
    std::uint8_t mode;
    std::uint16_t priority;
    float nominalVoltageVolts;
    float nominalCurrentAmps;
    float branchMaximumCurrentAmps;
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

static_assert(std::is_trivially_copyable<ConfigureLoadCommandPacket>::value,
              "ConfigureLoadCommandPacket must be safe to send over ESP-NOW");
static_assert(std::is_trivially_copyable<ConfigureLoadAcknowledgementPacket>::value,
              "ConfigureLoadAcknowledgementPacket must be safe to send over ESP-NOW");

} // namespace kilowatts

#endif // KILOWATTS_HARDWARE_CONFIGURATION_PACKETS_H
