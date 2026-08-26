/**
 * @file HardwareConfigurationPackets.h
 * @brief ESP-NOW packets for adding and removing Loads on a Node.
 *
 * These packets describe hardware/configuration data only.
 *
 * Load model carried over ESP-NOW:
 * - name
 * - relay pin
 * - relay active level
 * - mode
 * - power type
 * - priority
 * - power rating in watts
 * - optional Auto schedule window
 *
 * Do not add old fields such as nominal voltage, nominal current,
 * or startup watts. Those are not part of the current Load model.
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
    INVALID_POWER_RATING = 4U,
    INVALID_CONFIGURATION = 5U,
    HARDWARE_INITIALIZATION_FAILED = 6U,
    PERSISTENCE_FAILED = 7U,
    CAPACITY_REACHED = 8U
};

/**
 * @brief Configure or replace one Load on the destination Node.
 *
 * relayActiveHigh is a hardware property of the relay channel.
 * powerType is the serialized LoadPowerType value.
 * mode is the serialized LoadMode::Value value.
 */
struct ConfigureLoadCommandPacket {
    std::uint32_t commandId;

    char loadName[16];

    std::uint8_t relayPin;
    std::uint8_t relayActiveHigh;

    std::uint8_t mode;
    std::uint8_t powerType;

    std::uint16_t priority;

    float powerRatingWatts;

    std::uint8_t scheduleEnabled;
    std::uint8_t scheduleStartHour;
    std::uint8_t scheduleStartMinute;
    std::uint8_t scheduleEndHour;
    std::uint8_t scheduleEndMinute;
};

/** @brief Result of applying a ConfigureLoadCommandPacket. */
struct ConfigureLoadAcknowledgementPacket {
    std::uint32_t commandId;
    std::uint8_t relayPin;
    std::uint8_t success;
    std::uint8_t failureReason;
};

/** @brief Remove one Load from the destination Node. */
struct RemoveLoadCommandPacket {
    std::uint32_t commandId;
    std::uint8_t relayPin;
};

/** @brief Result of applying a RemoveLoadCommandPacket. */
struct RemoveLoadAcknowledgementPacket {
    std::uint32_t commandId;
    std::uint8_t relayPin;
    std::uint8_t success;
    std::uint8_t failureReason;
};

static_assert(
    std::is_trivially_copyable<ConfigureLoadCommandPacket>::value,
    "ConfigureLoadCommandPacket must be safe for ESP-NOW");

static_assert(
    std::is_trivially_copyable<ConfigureLoadAcknowledgementPacket>::value,
    "ConfigureLoadAcknowledgementPacket must be safe for ESP-NOW");

static_assert(
    std::is_trivially_copyable<RemoveLoadCommandPacket>::value,
    "RemoveLoadCommandPacket must be safe for ESP-NOW");

static_assert(
    std::is_trivially_copyable<RemoveLoadAcknowledgementPacket>::value,
    "RemoveLoadAcknowledgementPacket must be safe for ESP-NOW");

} // namespace kilowatts

#endif // KILOWATTS_HARDWARE_CONFIGURATION_PACKETS_H
