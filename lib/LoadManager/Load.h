/**
 * @file Load.h
 * @brief Defines one electrical load connected to a Central Node or Smart Node.
 *
 * A Load's ID (Node MAC address + relay pin) lets it keep its identity
 * when Loads from many Nodes are combined, processed and later sent back
 * through the network.
 */

#ifndef KILOWATTS_LOAD_H
#define KILOWATTS_LOAD_H

#include <array>
#include <cstddef>
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
 * Identifies the electrical supply required by a Load.
 *
 * The paper creates different child states by adding either an AC Load to
 * an AC source or a DC Load to a DC source.
 */
enum class LoadPowerType : std::uint8_t {
    AC = 0U,
    DC = 1U
};


/**
 * Preferred running window for an AUTO Load.
 *
 * Example:
 * {true, 6, 0, 8, 0} means the schedule is active from 06:00 up to 08:00.
 * {true, 22, 0, 2, 0} represents an overnight window from 22:00 to 02:00.
 *
 * enabled = false means no preferred running window has been set.
 */
struct AutoSchedule {
    bool enabled;
    std::uint8_t startHour;
    std::uint8_t startMinute;
    std::uint8_t endHour;
    std::uint8_t endMinute;
};


/**
 * Represents one electrical load connected to one ESP32 Node.
 *
 * Load models load configuration and planning data only: identity, name,
 * power rating, priority, power type, configured mode, optional
 * schedule, and the last Best-First Search rejection reason. getMode()/
 * setMode() is the CONFIGURED user/system operating mode
 * (FIXED_ON/FIXED_OFF/AUTO_ON/AUTO_OFF) — a setting, changed only by the
 * user (via LoadConfigurationStore) or by a Node's own report of its
 * locally configured state. Best-First Search's selection/rejection
 * result for an Auto Load must NEVER be written here — selecting an
 * AUTO_OFF Load for admission this cycle does not make it "become
 * AUTO_ON"; it stays configured AUTO_OFF. isOn()/isOff() below read this
 * configured value only, never a per-cycle planning decision.
 *
 * Load deliberately does NOT store a target or confirmed relay/GPIO
 * state. This firmware controls GPIO pins; it does not know what
 * physical device is wired to a pin, and an ESP-NOW acknowledgement only
 * means a Smart Node applied a GPIO command, never that a physical relay
 * moved, an appliance switched, or current is flowing. Each planning
 * cycle recomputes the desired ON/OFF state for every Load directly from
 * its configured mode plus BestFirstSearch's admission decision and
 * dispatches it immediately — there is nothing to persist here.
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
        float powerRatingWatts,
        std::uint16_t priority,
        LoadPowerType powerType,
        LoadMode::Value mode
    );


    /** Returns the complete Load ID: Node MAC address + relay pin. */
    const Id& getId() const;


    /** Returns the MAC address of the Node to which this Load belongs. */
    const MacAddress& getMacAddress() const;


    /** Returns the relay pin controlling this Load inside its Node. */
    std::uint8_t getRelayPin() const;


    /** Changes the user-defined name of this Load. */
    void setName(const std::string& name);


    /** Returns the user-defined name of this Load. */
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
     * Records the raw rejection-reason byte BestFirstSearch produced the
     * last time this Load was evaluated as an Auto candidate.
     */
    void setLastBestFirstRejectionReason(std::uint8_t reason);

    /** Returns the raw rejection-reason byte set by setLastBestFirstRejectionReason(). */
    std::uint8_t getLastBestFirstRejectionReason() const;


    /**
     * Changes the appliance power rating used by Best-First Search.
     * Returns false when powerRatingWatts is negative or not finite.
     */
    bool setPowerRatingWatts(float powerRatingWatts);


    /** Returns the appliance power rating used by Best-First Search. */
    float getPowerRatingWatts() const;


    /** Changes whether this Load requires an AC or DC power source. */
    void setPowerType(LoadPowerType powerType);


    /** Returns whether this Load requires an AC or DC power source. */
    LoadPowerType getPowerType() const;


    /** Changes the priority selected by the user. */
    void setPriority(std::uint16_t priority);


    /** Returns the priority selected by the user. */
    std::uint16_t getPriority() const;


    /**
     * Sets the preferred running window for an AUTO Load.
     *
     * startHour/endHour must be 0 to 23.
     * startMinute/endMinute must be 0 to 59.
     * Start and end must not be the same time. End may be earlier than
     * start to represent a window that crosses midnight.
     *
     * Returns false when:
     * - the Load is not Auto, or
     * - the schedule window is invalid.
     */
    bool setSchedule(AutoSchedule schedule);


    /** Returns the AUTO schedule. */
    AutoSchedule getSchedule() const;


    /** Removes the preferred AUTO running window. */
    void clearSchedule();


private:

    /** Permanent identity of this Load. */
    Id id_;

    /** Name selected by the user when registering the Load. */
    std::string name_;

    /** Raw BestFirstSearch rejection-reason byte from the last evaluation. */
    std::uint8_t lastBestFirstRejectionReason_;

    /** Appliance power rating in watts used by Best-First Search. */
    float powerRatingWatts_;

    /** AC or DC power-source requirement used when generating child states. */
    LoadPowerType powerType_;

    /** Priority selected by the user. */
    std::uint16_t priority_;

    /** Configured Fixed/Auto and ON/OFF state. */
    LoadMode::Value mode_;

    /** Preferred running window for an AUTO Load. */
    AutoSchedule schedule_;
};


/**
 * Writes mac as "%02X:%02X:%02X:%02X:%02X:%02X" into buffer.
 */
void formatMacAddressText(char* buffer, std::size_t bufferSize, const Load::MacAddress& mac);


} // namespace kilowatts

#endif // KILOWATTS_LOAD_H
