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
 * Installer-verified electrical ratings for a Load. These are planning
 * inputs (nameplate or commissioning-test values), never a claim that the
 * individual Load is currently being measured. The final hardware design
 * has the single INA219 at Central's battery bus only.
 */
struct LoadElectricalRatings {
    float nominalVoltageVolts;
    float nominalCurrentAmps;
};


/**
 * A Load's physical health as last reported by the Node that owns it.
 *
 * This is deliberately separate from LoadMode (the configured intent):
 * AVAILABLE/FAULTED/UNAVAILABLE describes whether the physical relay can
 * currently be trusted to do what it is told, not whether it is
 * configured Fixed/Auto or ON/OFF.
 */
enum class LoadHealth : std::uint8_t {
    AVAILABLE = 0U,     // Healthy; may be commanded normally.
    FAULTED = 1U,        // A relay command failed to confirm; needs maintenance.
    UNAVAILABLE = 2U     // Configuration invalid, or the owning hardware is not responding.
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
 *
 * State authority: this class deliberately keeps several distinct
 * concepts that must never overwrite one another (Section "State
 * Authority After These Corrections"):
 *
 * - getMode()/setMode() — the CONFIGURED user/system operating mode
 *   (FIXED_ON/FIXED_OFF/AUTO_ON/AUTO_OFF). This is a setting, changed
 *   only by the user (via LoadConfigurationStore) or by a Node's own
 *   report of its locally configured state. Best-First Search's
 *   selection/rejection result for an Auto Load must NEVER be written
 *   here — selecting AUTO_OFF Load for admission this cycle does not
 *   make it "become AUTO_ON"; it stays configured AUTO_OFF with a target
 *   of ON this cycle. isOn()/isOff() below read this configured value
 *   only, not the planning target.
 * - getTargetRelayState()/setTargetRelayState() — the relay state
 *   Central's CURRENT planning cycle wants: derived from Fixed
 *   configuration (or a critical-protection override) for a Fixed Load,
 *   or from BestFirstSearch's admission decision for an Auto Load.
 *   Recomputed every planning cycle.
 * - getConfirmedRelayState()/setConfirmedRelayState() plus
 *   isConfirmedRelayStateValid() — the last relay state actually
 *   confirmed by hardware read-back, and whether that confirmation can
 *   currently be trusted. A failed/timed-out relay command must never
 *   force this to OFF or mark it invalid — see setConfirmedRelayState().
 * - getHealth()/setHealth() — whether this Load can currently be
 *   trusted to safely accept new commands (AVAILABLE/FAULTED/UNAVAILABLE).
 *
 * Reporting/MQTT/tree code must expose all of these separately so the
 * mobile application (and this project's own diagnostics) can always
 * tell "what the user configured" apart from "what we currently want"
 * apart from "what we last knew for certain."
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
        LoadMode::Value mode,
        LoadElectricalRatings electricalRatings = LoadElectricalRatings{0.0F, 0.0F}
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
     * Records the relay state actually read back from hardware
     * (RelayController::readBackState()) after a successfully confirmed
     * relay command. This is the physical truth, distinct from
     * getMode()/isOn() (the configured/commanded intent) and from
     * getTargetRelayState() (what the current planning cycle wants) — a
     * caller must not treat any of the three as another.
     *
     * Calling this always marks the confirmation valid
     * (isConfirmedRelayStateValid() becomes true): only call it when a
     * command has genuinely been confirmed. A failed/timed-out command
     * must NOT call this — see the class-level documentation and
     * RelayCommandDispatcher's README for why a failed acknowledgement
     * does not prove the relay is OFF.
     */
    void setConfirmedRelayState(bool on);

    /**
     * Returns the last relay state confirmed by hardware read-back — the
     * last value ever passed to setConfirmedRelayState(), preserved
     * exactly as-is across any number of failed commands in between.
     * Always check isConfirmedRelayStateValid() first: when it is false,
     * this value is stale/unknown and must not be trusted as current
     * physical truth (though it may still be a useful conservative
     * estimate — see Load's class documentation).
     */
    bool getConfirmedRelayState() const;

    /**
     * Returns whether getConfirmedRelayState() currently reflects a
     * trustworthy physical confirmation. False from construction until
     * the first setConfirmedRelayState() call, and stays exactly as it
     * was through any number of failed/timed-out commands afterward —
     * only a fresh successful confirmation ever makes it true again
     * (there is no separate "invalidate" call: failure simply never
     * touches this flag, which is what "preserve the previous trusted
     * state" means in practice).
     */
    bool isConfirmedRelayStateValid() const;


    /**
     * Records targetOn as the relay state Central's current planning
     * cycle wants for this Load — derived from Fixed configuration (or
     * documented critical-protection override) for a Fixed Load, or from
     * BestFirstSearch's admission decision for an Auto Load. This is
     * planning output, recomputed every cycle; it is deliberately NEVER
     * written into getMode()/setMode() — an Auto Load's configured
     * AUTO_ON/AUTO_OFF mode is a user/system setting that Best-First
     * Search must never overwrite merely because it selected or rejected
     * the Load this cycle (see Load's class documentation).
     */
    void setTargetRelayState(bool targetOn);

    /**
     * Returns the relay state the current planning cycle wants for this
     * Load. Undefined/stale before the first setTargetRelayState() call
     * this boot — callers that run after the planning cycle has executed
     * at least once may rely on it.
     */
    bool getTargetRelayState() const;


    /** Records this Load's current physical health. */
    void setHealth(LoadHealth health);

    /** Returns this Load's current physical health. */
    LoadHealth getHealth() const;


    /**
     * Records the raw rejection-reason byte BestFirstSearch produced the
     * last time this Load was evaluated as an Auto candidate (see
     * BestFirstSearch::NONE/LOW_BATTERY/... in BestFirstSearch.h). Stored
     * as a plain byte, not BestFirstSearch's type, so this header never
     * depends on BestFirstSearch.h — Load is a leaf dependency of that
     * class, never the reverse. 0 (BestFirstSearch::NONE) means either
     * "admitted" or "never evaluated this cycle" (for example a Fixed
     * Load, which Best-First Search never ranks).
     */
    void setLastBestFirstRejectionReason(std::uint8_t reason);

    /** Returns the raw rejection-reason byte set by setLastBestFirstRejectionReason(). */
    std::uint8_t getLastBestFirstRejectionReason() const;


    /**
     * Changes the normal running power and startup power.
     *
     * startupWatts must not be lower than runningWatts.
     */
    bool setPower(LoadPower power);


    /** Returns the configured power values. */
    LoadPower getPower() const;


    /**
     * Stores the installer/nameplate voltage/current pair used to derive the
     * configured running power. A zero/zero pair is permitted only for legacy
     * loads that have no such rating; a real runtime configuration requires
     * both values to be positive.
     */
    bool setElectricalRatings(LoadElectricalRatings electricalRatings);

    /** Returns the configured installer/nameplate electrical ratings. */
    LoadElectricalRatings getElectricalRatings() const;


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


    /** Last relay state confirmed by hardware read-back (physical truth). */
    bool confirmedRelayState_;


    /** Whether confirmedRelayState_ currently reflects a trustworthy confirmation. */
    bool confirmedRelayStateValid_;


    /** Relay state the current planning cycle wants (Central's target, not the configured mode). */
    bool targetRelayState_;


    /** Current physical health as last reported by the owning Node. */
    LoadHealth health_;


    /** Raw BestFirstSearch rejection-reason byte from the last evaluation. */
    std::uint8_t lastBestFirstRejectionReason_;


    /** Normal running power and startup power. */
    LoadPower power_;


    /** Installer/nameplate ratings, distinct from measurements_. */
    LoadElectricalRatings electricalRatings_;


    /** Priority selected by the user. */
    std::uint16_t priority_;


    /**
     * The ONLY stored value that determines CONFIGURED Fixed/Auto and
     * ON/OFF — never the planning target (see targetRelayState_) and
     * never mutated by Best-First Search's per-cycle selection result.
     */
    LoadMode::Value mode_;


    /** Preferred running time for an AUTO Load. */
    AutoSchedule schedule_;
};


} // namespace kilowatts

#endif // KILOWATTS_LOAD_H
