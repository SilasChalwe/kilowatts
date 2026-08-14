/**
 * @file RelayController.h
 * @brief Declares local relay GPIO actuation for the Loads wired to THIS Node.
 *
 * RelayController's one responsibility: drive the physical relay GPIO pins
 * this Node owns, and report back the commanded/read-back state honestly.
 * It knows nothing about ESP-NOW, Node topology, Best-First Search, or
 * which Load a relay pin happens to represent in the wider system — the
 * caller (the Smart/Central Node's Relay Control Task) supplies a relay
 * pin and a desired ON/OFF state, exactly as Load::getRelayPin() already
 * identifies that Load inside its owning Node.
 *
 * A relay module's ON signal polarity depends on the physical relay board
 * wired to a given pin (many opto-isolated relay boards are active-LOW),
 * so RelayConfiguration::activeHigh is always supplied explicitly by the
 * caller rather than assumed — the same "never invent an undocumented
 * hardware default" convention INA219Monitor already follows for shunt
 * resistance and expected current.
 *
 * This header has no ESP-IDF dependency so it can be included, and its
 * relay-registration/state-tracking logic exercised, from a plain host
 * build (see test/RelayController/). The real GPIO register access is
 * compiled only when building for the ESP32 target (see
 * RelayController.cpp), matching the ESP_PLATFORM split already used by
 * INA219Monitor and CurrentTimeProvider.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
 */

#ifndef KILOWATTS_RELAY_CONTROLLER_H
#define KILOWATTS_RELAY_CONTROLLER_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kilowatts {


/**
 * Manages every relay channel physically wired to THIS Node and actuates
 * the GPIO pin controlling each one.
 *
 * Usage:
 *
 *     RelayController relays;
 *     relays.addRelay({16U, /-* activeHigh *-/ false, /-* initialStateOn *-/ false});
 *     relays.setRelayState(16U, true);   // switch that Load ON
 *     bool on = false;
 *     relays.getCommandedState(16U, on);
 */
class RelayController {

public:

    /**
     * Configuration for one physical relay channel.
     *
     * relayPin is the same relay pin identity used throughout the system
     * (Load::getRelayPin(), BestFirstSearch::BranchId::relayPin) — the
     * GPIO number on THIS Node's ESP32 that drives that relay channel.
     *
     * activeHigh describes the physical relay board wired to this pin:
     * true when driving the GPIO HIGH energises the relay (Load ON),
     * false when the board is active-LOW (driving the GPIO LOW energises
     * the relay). This is a property of the physical board, never
     * assumed, so it is always supplied explicitly.
     *
     * initialStateOn is the safe state applied the moment this relay is
     * registered — normally false (OFF), matching the document's
     * pull-down/safe-default boot behaviour. The GPIO is configured for
     * output and driven to this state in the same call, before any other
     * code can command it, so there is no undefined transient state.
     */
    struct RelayConfiguration {
        std::uint8_t relayPin;
        bool activeHigh;
        bool initialStateOn;
    };


    RelayController();

    ~RelayController();

    /*
     * Copying would duplicate ownership of the underlying GPIO pins this
     * object has configured, so RelayController is not copyable.
     */
    RelayController(const RelayController&) = delete;
    RelayController& operator=(const RelayController&) = delete;


    /**
     * Registers one relay channel and immediately configures its GPIO as
     * an output driven to initialStateOn.
     *
     * Rejected (returns false, no GPIO is touched) when relayPin is
     * already registered. On the ESP32 target, also rejected when the
     * underlying GPIO configuration call fails; on a host build, the
     * registration/state bookkeeping still succeeds so the relay-command
     * pipeline above this class can be exercised without hardware, but
     * isHardwareApplied() reports false for that channel.
     */
    bool addRelay(const RelayConfiguration& configuration);


    /** Returns how many relay channels are currently registered. */
    std::size_t getNumberOfRelays() const;

    /** Returns nullptr when relayIndex does not exist. */
    const RelayConfiguration* getRelay(std::size_t relayIndex) const;

    /** Returns true when relayPin has been registered via addRelay(). */
    bool isRelayRegistered(std::uint8_t relayPin) const;


    /**
     * Commands relayPin to the requested ON/OFF state.
     *
     * Returns false when relayPin is not registered, or (on the ESP32
     * target) when the GPIO write itself failed. The commanded state is
     * still recorded even when the underlying write fails, so a caller
     * that also calls readBackState() can detect the mismatch and treat
     * the relay as faulted, rather than the two bookkeeping values
     * silently disagreeing forever.
     */
    bool setRelayState(std::uint8_t relayPin, bool on);


    /**
     * Returns the last state this object commanded for relayPin (not a
     * hardware read-back). Returns false, and leaves on unchanged, when
     * relayPin is not registered.
     */
    bool getCommandedState(std::uint8_t relayPin, bool& on) const;


    /**
     * Reads the real GPIO output level currently driven for relayPin and
     * translates it back into a logical ON/OFF state using that relay's
     * activeHigh polarity. This is the read-back the document requires
     * before a relay command is reported as confirmed.
     *
     * Returns false, and leaves on unchanged, when relayPin is not
     * registered, or on a host build where no real GPIO exists.
     */
    bool readBackState(std::uint8_t relayPin, bool& on) const;


    /**
     * Returns true when relayPin's GPIO was actually configured on real
     * hardware (always false on a host build, even though registration
     * and commanded-state tracking still work there).
     */
    bool isHardwareApplied(std::uint8_t relayPin) const;


    /** Prints one diagnostic table row per registered relay channel. */
    void printDiagnosticReport() const;


private:

    struct RegisteredRelay {
        RelayConfiguration configuration;
        bool commandedOn;
        bool hardwareApplied;
    };


    RegisteredRelay* findMutableRelay(std::uint8_t relayPin);
    const RegisteredRelay* findRelay(std::uint8_t relayPin) const;

    /**
     * Configures relay's GPIO as a safe-default output. Implemented only
     * under ESP_PLATFORM; the host build's implementation always returns
     * false without touching any hardware.
     */
    bool configureGpio(RegisteredRelay& relay);

    /**
     * Writes the real GPIO level corresponding to logical state on for
     * relay, honouring its activeHigh polarity. Implemented only under
     * ESP_PLATFORM; the host build's implementation always returns false.
     */
    bool writeGpioLevel(const RegisteredRelay& relay, bool on) const;

    /**
     * Reads the real GPIO output level for relay and translates it back
     * into a logical ON/OFF state. Implemented only under ESP_PLATFORM;
     * the host build's implementation always returns false.
     */
    bool readGpioLevel(const RegisteredRelay& relay, bool& on) const;


    std::vector<RegisteredRelay> relays_;
};


} // namespace kilowatts

#endif // KILOWATTS_RELAY_CONTROLLER_H
