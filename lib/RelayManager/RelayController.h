/**
 * @file RelayController.h
 * @brief Declares local GPIO actuation for the Load-control outputs on THIS Node.
 *
 * RelayController's one responsibility is to configure and drive the GPIO
 * pins used by the prototype's relay/driver hardware. It does not verify the
 * behaviour of anything connected after the ESP32 GPIO.
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
 * This header has no ESP-IDF dependency so its registration logic can be
 * included in host-native tests. GPIO configuration/writes are compiled only
 * for ESP32 builds in RelayController.cpp.
 */

#ifndef KILOWATTS_RELAY_CONTROLLER_H
#define KILOWATTS_RELAY_CONTROLLER_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kilowatts {


/**
 * Manages every configured Load-control GPIO on THIS Node.
 *
 * Usage:
 *
 *     RelayController relays;
 *     relays.addRelay({16U, /-* activeHigh *-/ false, /-* initialStateOn *-/ false});
 *     relays.setRelayState(16U, true);   // request the configured output ON
 */
class RelayController {

public:

    /**
     * Configuration for one GPIO control channel.
     *
     * relayPin is the GPIO number on THIS Node's ESP32 used for this Load.
     * The owning Node MAC address plus relayPin form the complete Load::Id.
     *
     * activeHigh describes the electrical polarity expected by the connected
     * driver: true means logical ON is GPIO HIGH; false means logical ON is
     * GPIO LOW. The software does not infer what device is connected.
     *
     * initialStateOn is the safe state applied the moment this relay is
     * registered — normally false (OFF), matching the pull-down/safe-
     * default boot behaviour. The GPIO is configured for
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
     * Registers one control channel and immediately configures its GPIO as
     * an output driven to initialStateOn.
     *
     * Rejected (returns false, no GPIO is touched) when relayPin is
     * already registered. On the ESP32 target, also rejected when the
     * underlying GPIO configuration call fails; on a host build, the
     * registration still succeeds so non-hardware logic can be exercised,
     * but isHardwareApplied() reports false for that channel.
     */
    bool addRelay(const RelayConfiguration& configuration);


    /**
     * Removes a control channel that was provisionally registered during
     * a failed hardware-configuration transaction. The GPIO is driven to
     * logical OFF before its GPIO is released on ESP32 builds. Normal
     * operational code does not remove live loads; this exists so a failed
     * installer command cannot leave an orphaned, untracked output behind.
     */
    bool removeRelay(std::uint8_t relayPin);


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
     * target) when the GPIO write itself failed. Success means only that
     * the ESP-IDF GPIO write call succeeded; it says nothing about a
     * downstream device connected to the pin.
     */
    bool setRelayState(std::uint8_t relayPin, bool on);


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

    std::vector<RegisteredRelay> relays_;
};


} // namespace kilowatts

#endif // KILOWATTS_RELAY_CONTROLLER_H
