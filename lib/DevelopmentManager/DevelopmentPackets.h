/**
 * @file DevelopmentPackets.h
 * @brief Wire-format structures for the Development Session and Factory
 *        Reset commands, sent between Kilowatts ESP32 firmware over
 *        ESP-NOW.
 *
 * Transport-only structures, the same role CommissioningPackets already
 * fills for the commissioning lifecycle — kept in their own file for the
 * same reason: small, sent rarely (only while an operator is actively
 * driving a Development Session or performing a factory reset), and a
 * distinct concern from the hot NodeReportPacket/RelayCommandPacket
 * cycle.
 *
 * Central is always the sender of every *_COMMAND packet here and the
 * receiver of every *_ACK; the owning Node is always the surrounding
 * EspNowCommunication::Message header's origin/destination MAC address,
 * exactly like every other packet in this project — never repeated
 * inside these structs.
 */

#ifndef KILOWATTS_DEVELOPMENT_PACKETS_H
#define KILOWATTS_DEVELOPMENT_PACKETS_H

#include "EspNowCommunication.h"

#include <cstdint>
#include <type_traits>

namespace kilowatts {


enum class DevSessionAction : std::uint8_t {
    START = 0U,
    END = 1U
};


/** Central -> Node: start or end that Node's own local Development Session. */
struct DevSessionCommandPacket {
    std::uint32_t commandId;
    std::uint8_t action;   // raw DevSessionAction byte
};


/**
 * Central -> Node: arm or clear a simulated raw voltage/current override
 * for the sensor at i2cAddress. clearOverride selects which:
 * clearOverride=0 arms voltageVolts/currentAmps as an override (the Node
 * registers a temporary sensor slot for i2cAddress first if it does not
 * already have one, exactly like INA219Monitor::addSensor() +
 * setDevelopmentOverride() — see each role's own main.cpp handler);
 * clearOverride=1 removes the override for i2cAddress.
 */
struct DevSensorInputCommandPacket {
    std::uint32_t commandId;
    std::uint8_t i2cAddress;
    std::uint8_t clearOverride;
    float voltageVolts;
    float currentAmps;
};


/** resultingEnvironment always reflects the Node's true OperatingEnvironment after processing the command, whether accepted or rejected - the same "always honest, not only on success" convention CommissionAckPacket::resultingState already uses. */
struct DevAckPacket {
    std::uint32_t commandId;
    std::uint8_t success;
    std::uint8_t resultingEnvironment;   // raw OperatingEnvironment byte
};


/**
 * Central -> Node: erase this Node's own installation-specific persisted
 * configuration and reboot into PRODUCTION + UNCOMMISSIONED. confirmToken
 * must equal FACTORY_RESET_CONFIRM_TOKEN exactly — a mismatched token is
 * rejected without touching NVS.
 */
constexpr std::uint8_t FACTORY_RESET_CONFIRM_TOKEN = 0x5AU;

struct FactoryResetCommandPacket {
    std::uint32_t commandId;
    std::uint8_t confirmToken;
};


/**
 * Sent only when the confirm token was wrong or NVS erase failed - a
 * successful factory reset reboots the Node immediately instead of
 * replying, since there is nothing meaningful left to acknowledge from
 * (the Node is about to forget it was ever commissioned, including any
 * route back to Central).
 */
struct FactoryResetAckPacket {
    std::uint32_t commandId;
    std::uint8_t success;
};


static_assert(std::is_trivially_copyable<DevSessionCommandPacket>::value,
              "DevSessionCommandPacket must be safe to send and receive over ESP-NOW");
static_assert(std::is_trivially_copyable<DevSensorInputCommandPacket>::value,
              "DevSensorInputCommandPacket must be safe to send and receive over ESP-NOW");
static_assert(std::is_trivially_copyable<DevAckPacket>::value,
              "DevAckPacket must be safe to send and receive over ESP-NOW");
static_assert(std::is_trivially_copyable<FactoryResetCommandPacket>::value,
              "FactoryResetCommandPacket must be safe to send and receive over ESP-NOW");
static_assert(std::is_trivially_copyable<FactoryResetAckPacket>::value,
              "FactoryResetAckPacket must be safe to send and receive over ESP-NOW");

static_assert(sizeof(DevSessionCommandPacket) <= EspNowCommunication::MAX_PAYLOAD_SIZE,
              "DevSessionCommandPacket is too large to fit inside one ESP-NOW message");
static_assert(sizeof(DevSensorInputCommandPacket) <= EspNowCommunication::MAX_PAYLOAD_SIZE,
              "DevSensorInputCommandPacket is too large to fit inside one ESP-NOW message");
static_assert(sizeof(DevAckPacket) <= EspNowCommunication::MAX_PAYLOAD_SIZE,
              "DevAckPacket is too large to fit inside one ESP-NOW message");
static_assert(sizeof(FactoryResetCommandPacket) <= EspNowCommunication::MAX_PAYLOAD_SIZE,
              "FactoryResetCommandPacket is too large to fit inside one ESP-NOW message");
static_assert(sizeof(FactoryResetAckPacket) <= EspNowCommunication::MAX_PAYLOAD_SIZE,
              "FactoryResetAckPacket is too large to fit inside one ESP-NOW message");


} // namespace kilowatts

#endif // KILOWATTS_DEVELOPMENT_PACKETS_H
