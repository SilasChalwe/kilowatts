/**
 * @file CentralNodeConfig.h
 * @brief Compile-time Central Node constants that are independent of one
 *        installation's battery/load configuration.
 *
 * Installation values such as battery capacity, minimum SoC, maximum battery
 * discharge current and maximum main-bus current are persisted through
 * CentralConfigurationStore. They are not hard-coded here.
 */
#ifndef KILOWATTS_CENTRAL_NODE_CONFIG_H
#define KILOWATTS_CENTRAL_NODE_CONFIG_H

#include <cstdint>

namespace kilowatts {
namespace CentralNodeConfig {

constexpr const char* CENTRAL_NODE_NAME = "Central";
constexpr const char* WIFI_STATION_HOSTNAME = "kilowatts-central";

/*
 * Central battery-bus I2C wiring.
 *
 * PowerManager::initialize() receives these values through its
 * BusConfiguration. PowerManager's private initializeBus() is never called
 * directly by Central runtime code.
 */
constexpr std::uint8_t I2C_SERIAL_DATA_PIN = 21U;
constexpr std::uint8_t I2C_SERIAL_CLOCK_PIN = 22U;
constexpr std::uint8_t I2C_PORT_NUMBER = 0U;
constexpr std::uint32_t I2C_CLOCK_SPEED_HZ = 400000U;

/*
 * Used only when a first-time serial-console battery configuration has not
 * supplied/persisted another battery nameplate voltage yet.
 *
 * This is a nameplate/system voltage, not a live INA219 reading and not a
 * charger "full" voltage.
 */
constexpr float NOMINAL_BATTERY_VOLTAGE_VOLTS = 12.0F;

/* Runtime task periods. */
constexpr std::uint32_t DEFAULT_OPTIMIZATION_PERIOD_MILLISECONDS = 300000U;
constexpr std::uint32_t MIN_OPTIMIZATION_PERIOD_MILLISECONDS = 5000U;
constexpr std::uint32_t MAX_OPTIMIZATION_PERIOD_MILLISECONDS = 86400000U;
constexpr std::uint32_t OPTIMIZATION_PERIOD_MILLISECONDS = DEFAULT_OPTIMIZATION_PERIOD_MILLISECONDS;
constexpr std::uint32_t SENSOR_ACQUISITION_PERIOD_MILLISECONDS = 1000U;
constexpr std::uint32_t WATCHDOG_PERIOD_MILLISECONDS = 60000U;
constexpr std::uint32_t RELAY_COMMAND_ACK_TIMEOUT_MILLISECONDS = 3000U;
constexpr std::uint32_t RELAY_ON_FIRST_DELAY_MILLISECONDS = 3000U;
constexpr std::uint32_t RELAY_ON_BETWEEN_DELAY_MILLISECONDS = 2000U;

/* Smart Node report freshness. */
constexpr std::uint32_t NODE_REPORT_TIMEOUT_MILLISECONDS = 10000U;

/* Battery telemetry freshness. */
constexpr std::uint32_t BATTERY_TELEMETRY_STALE_TIMEOUT_MILLISECONDS = 5000U;

/*
 * MQTT namespace.
 *
 * A deployment may override this with:
 * -DKILOWATTS_MQTT_TOPIC_NAMESPACE=\"kilowatts/v1/home-42\"
 */
#ifndef KILOWATTS_MQTT_TOPIC_NAMESPACE
#define KILOWATTS_MQTT_TOPIC_NAMESPACE "kilowatts/v1"
#endif

constexpr const char* MQTT_TOPIC_NAMESPACE =
    KILOWATTS_MQTT_TOPIC_NAMESPACE;

constexpr const char* MQTT_DEVICE_ID = "central-01";
constexpr std::uint32_t MQTT_SCHEMA_VERSION = 3U;

} // namespace CentralNodeConfig
} // namespace kilowatts

#endif // KILOWATTS_CENTRAL_NODE_CONFIG_H