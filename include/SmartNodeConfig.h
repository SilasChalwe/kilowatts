/**
 * @file SmartNodeConfig.h
 * @brief Genuine hardware/board configuration for Kilowatts Smart Node
 *        firmware — no installation-specific Loads/Branches/sensors.
 *
 * This header is compile-time board configuration only: real per-board
 * wiring facts (which GPIOs are the I2C bus on this board) that apply to
 * every physical Smart Node flashed with this firmware, regardless of
 * where it ends up installed. It intentionally does NOT define any Load,
 * relay assignment, INA219 address, or Node identity/name — a freshly
 * flashed Smart Node knows nothing about the installation it will serve
 * until it is discovered and commissioned over ESP-NOW (see
 * lib/NodeIdentityStore, lib/CommissioningPackets). Installation topology
 * (Branches, Loads, sensors) is a later phase's runtime-configured,
 * NVS-persisted concern, never a compiled-in constant here.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 14 August 2026
 */

#ifndef KILOWATTS_SMART_NODE_CONFIG_H
#define KILOWATTS_SMART_NODE_CONFIG_H

#include "INA219Monitor.h"

#include <cstdint>

namespace kilowatts {
namespace SmartNodeConfig {


/*
 * -----------------------------------------------------------------------
 * I2C bus (PENDING HARDWARE VERIFICATION)
 * -----------------------------------------------------------------------
 * Several GPIOs on an ESP32-S3 camera/CAM-MB board are already claimed by
 * the camera interface, so these pins must be re-checked against the
 * actual board's silkscreen/schematic before this firmware is relied on
 * with real I2C hardware attached. This is a physical board wiring fact
 * (the same status as RadioConfig.h's channel), not installation
 * configuration - it applies to every Smart Node built on this board,
 * independent of which sensors are later commissioned onto this bus.
 */
constexpr std::uint8_t I2C_SERIAL_DATA_PIN = 8U;
constexpr std::uint8_t I2C_SERIAL_CLOCK_PIN = 9U;
constexpr std::uint32_t I2C_CLOCK_SPEED_HZ = 400000U;
constexpr std::uint8_t I2C_PORT_NUMBER = 0U;

inline INA219Monitor::I2CBusConfiguration i2cBusConfiguration()
{
    return INA219Monitor::I2CBusConfiguration{
        I2C_SERIAL_DATA_PIN, I2C_SERIAL_CLOCK_PIN, I2C_CLOCK_SPEED_HZ, I2C_PORT_NUMBER
    };
}


} // namespace SmartNodeConfig
} // namespace kilowatts

#endif // KILOWATTS_SMART_NODE_CONFIG_H
