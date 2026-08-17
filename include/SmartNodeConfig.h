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
 * other GPIOs.
 *
 * This is a chip-level safe set for the ESP32-S3-WROOM-1 N16R8 module (16MB
 * flash + 8MB octal PSRAM, matching this Node's confirmed hardware), not a
 * per-unit continuity-tested list. It excludes every pin Espressif documents
 * as unsafe for this module: GPIO0/3/45/46 (strapping), GPIO43/44 (default
 * UART0 console), GPIO19/20 (default USB-JTAG), GPIO26-32 (shared SPI
 * flash/PSRAM bus) and GPIO35-37 (octal-PSRAM data lines on the R8
 * variant). Capped at MAX_RELAY_GPIO_CAPABILITIES (8) to match the
 * IdentityReportPacket wire format.
 *
 * This board is an ESP32-S3 camera module (ESP32-CAM-MB carrier) - if the
 * camera sensor itself is physically populated on a given unit, its DVP
 * bus/SIOD/SIOC/PWDN pins are additionally reserved on that unit even
 * though they are chip-safe in general; verify against that specific
 * camera module's schematic before wiring a relay to it.
 */
constexpr std::array<std::uint8_t, 8U> VERIFIED_RELAY_GPIO_PINS{
    4U, 5U, 6U, 7U, 15U, 16U, 17U, 18U
};

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
