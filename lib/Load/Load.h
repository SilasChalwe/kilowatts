/**
 * @file Load.h
 * @brief Defines one electrical load connected to a Central Node or Smart Node.
 *
 * Every Load belongs to one ESP32 Node.
 *
 * The Load ID is:
 *
 *     Node MAC address + Relay Pin
 *
 * This allows the Load to keep its identity when Loads from many Nodes
 * are combined, processed and later sent back through the network.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 8 May 2026
 */

#ifndef KILOWATTS_LOAD_H
#define KILOWATTS_LOAD_H

#include <array>
#include <cstdint>
#include <string>

namespace kilowatts {


/**
 * Defines how the load is controlled and whether it is ON or OFF.
 *
 * Examples:
 * LoadMode::Fixed::ON
 * LoadMode::Fixed::OFF
 * LoadMode::Auto::ON
 * LoadMode::Auto::OFF
 *
 * Only one value is stored in Load.
 */
struct LoadMode {

    enum class Value : std::uint8_t {
        FIXED_OFF = 0U,
        FIXED_ON  = 1U,
        AUTO_OFF  = 2U,
        AUTO_ON   = 3U
    };

    struct Fixed {
        static constexpr Value OFF = Value::FIXED_OFF;
        static constexpr Value ON  = Value::FIXED_ON;
    };

    struct Auto {
        static constexpr Value OFF = Value::AUTO_OFF;
        static constexpr Value ON  = Value::AUTO_ON;
    };
};


/**
 * Latest electrical measurements for one individual load.
 */
struct LoadMeasurements {
    float voltageVolts;
    float currentAmps;
    float powerWatts;
};


/**
 * Power values configured for one load.
 */
struct LoadPower {
    float runningWatts;
    float startupWatts;
};


/**
 * Preferred running time for an AUTO load.
 *
 * Example:
 * {true, 6, 0}  means 06:00.
 * {true, 20, 0} means 20:00.
 *
 * enabled = false means no preferred time has been set.
 */
struct AutoSchedule {
    bool enabled;
    std::uint8_t hour;
    std::uint8_t minute;
};


/**
 * Represents one electrical load connected to one ESP32 Node.
 */
class Load {

public:

    /**
     * One ESP32 MAC address contains six bytes.
     */
    using MacAddress = std::array<std::uint8_t, 6>;


    /**
     * Globally identifies one physical Load.
     *
     * The MAC address identifies the Node.
     * The relay pin identifies the Load inside that Node.
     */
    struct Id {
        MacAddress macAddress;
        std::uint8_t relayPin;
    };


    /**
     * Creates one Load.
     *
     * The MAC address is the MAC address of the Node
     * to which this Load is registered.
     */
    Load(
        const Id& id,
        const std::string& name,
        LoadPower power,
        std::uint16_t priority,
        LoadMode::Value mode
    );


    /**
     * Returns the complete Load ID:
     * Node MAC address + relay pin.
     */
    const Id& getId() const;


    /**
     * Returns the MAC address of the Node to which this Load belongs.
     */
    const MacAddress& getMacAddress() const;


    /**
     * Returns the relay pin controlling this Load inside its Node.
     */
    std::uint8_t getRelayPin() const;


    /**
     * Changes the user-defined name of this Load.
     */
    void setName(const std::string& name);


    /**
     * Returns the user-defined name of this Load.
     */
    const std::string& getName() const;


    /**
     * Changes the complete Fixed/Auto and ON/OFF value.
     *
     * Examples:
     * setMode(LoadMode::Fixed::ON);
     * setMode(LoadMode::Fixed::OFF);
     * setMode(LoadMode::Auto::ON);
     * setMode(LoadMode::Auto::OFF);
     */
    void setMode(LoadMode::Value mode);


    /** Returns the complete Fixed/Auto and ON/OFF value. */
    LoadMode::Value getMode() const;


    /** Returns true when the Load is Fixed. */
    bool isFixed() const;


    /** Returns true when the Load is Auto. */
    bool isAuto() const;


    /** Returns true when the Load is ON. */
    bool isOn() const;


    /** Returns true when the Load is OFF. */
    bool isOff() const;


    /**
     * Stores the latest voltage, current and instantaneous power
     * measured for this individual Load.
     */
    bool setMeasurements(LoadMeasurements measurements);


    /** Returns the latest measurements for this Load. */
    LoadMeasurements getMeasurements() const;


    /**
     * Changes the normal running power and startup power.
     *
     * startupWatts must not be lower than runningWatts.
     */
    bool setPower(LoadPower power);


    /** Returns the configured power values. */
    LoadPower getPower() const;


    /** Changes the priority selected by the user. */
    void setPriority(std::uint16_t priority);


    /** Returns the priority selected by the user. */
    std::uint16_t getPriority() const;


    /**
     * Sets the preferred running time for an AUTO Load.
     *
     * hour must be 0 to 23.
     * minute must be 0 to 59.
     *
     * Returns false when:
     * - the Load is not Auto, or
     * - the time is invalid.
     */
    bool setSchedule(AutoSchedule schedule);


    /** Returns the AUTO schedule. */
    AutoSchedule getSchedule() const;


    /** Removes the preferred AUTO running time. */
    void clearSchedule();


private:

    /**
     * Permanent identity of this Load.
     *
     * id_.macAddress identifies the ESP32 Node.
     * id_.relayPin identifies this Load inside that Node.
     */
    Id id_;


    /** Name selected by the user when registering the Load. */
    std::string name_;


    /** Latest voltage, current and instantaneous power. */
    LoadMeasurements measurements_;


    /** Normal running power and startup power. */
    LoadPower power_;


    /** Priority selected by the user. */
    std::uint16_t priority_;


    /**
     * The ONLY stored value that determines:
     * Fixed or Auto
     * and
     * ON or OFF.
     */
    LoadMode::Value mode_;


    /** Preferred running time for an AUTO Load. */
    AutoSchedule schedule_;
};


} // namespace kilowatts

#endif // KILOWATTS_LOAD_H