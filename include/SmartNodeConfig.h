/**
 * @file SmartNodeConfig.h
 * @brief Genuine hardware/board configuration for Kilowatts Smart Node
 *        firmware — no installation-specific Loads/Branches/sensors.
 *
 * This header is compile-time board configuration only: real per-board
 * wiring facts (which GPIOs safely drive relay inputs on this board) that
 * apply to every physical Smart Node flashed with this firmware, regardless
 * of where it ends up installed. It intentionally does NOT define any Load,
 * relay assignment, INA219 address, or Node identity/name — a freshly
 * flashed Smart Node knows nothing about the installation it will serve
 * until it is discovered and commissioned over ESP-NOW (see
 * lib/NodeIdentityStore, lib/CommissioningPackets). Installation topology
 * (Branches and Loads) is a runtime-configured, NVS-persisted concern,
 * never a compiled-in constant here. The one INA219 in the final hardware
 * design belongs at Central on the battery bus; Smart Nodes deliberately do
 * not configure an I2C bus or per-load current sensors.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 14 August 2026
 */

#ifndef KILOWATTS_SMART_NODE_CONFIG_H
#define KILOWATTS_SMART_NODE_CONFIG_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace kilowatts {
namespace SmartNodeConfig {


/*
 * -----------------------------------------------------------------------
 * Relay GPIO capability inventory
 * -----------------------------------------------------------------------
 * This is a board-safety contract, not installation topology. The installer
 * UI receives this exact list from the flashed Smart Node and offers no
 * other GPIOs. It is intentionally empty for the current ESP32-S3 camera
 * board until its schematic and the attached relay wiring have been
 * verified: guessing a GPIO could conflict with camera/flash/boot hardware
 * and energise a load unexpectedly. Once verified, add only tested output
 * pins here and reflash the common Smart Node image.
 */
constexpr std::array<std::uint8_t, 0U> VERIFIED_RELAY_GPIO_PINS{};

inline std::size_t getVerifiedRelayPinCount()
{
    return VERIFIED_RELAY_GPIO_PINS.size();
}

inline std::uint8_t getVerifiedRelayPin(std::size_t index)
{
    return index < VERIFIED_RELAY_GPIO_PINS.size() ? VERIFIED_RELAY_GPIO_PINS[index] : 0U;
}

inline bool isVerifiedRelayPin(std::uint8_t pin)
{
    for (const std::uint8_t candidate : VERIFIED_RELAY_GPIO_PINS) {
        if (candidate == pin) {
            return true;
        }
    }
    return false;
}


} // namespace SmartNodeConfig
} // namespace kilowatts

#endif // KILOWATTS_SMART_NODE_CONFIG_H
