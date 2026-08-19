/**
 * @file DevelopmentSession.h
 * @brief Declares the explicit, runtime-only Development Session a Node's
 *        Operating Environment can enter.
 *
 * Nothing in this class ever changes environment or arms an override on
 * its own — every transition is the direct, explicit result of a caller
 * (an MQTT command on Central, or a DEV_SESSION_COMMAND/
 * DEV_SENSOR_INPUT_COMMAND received over ESP-NOW on a Smart Node) calling
 * start()/end()/setSensorOverride()/clearSensorOverride(). This class
 * stores policy/state only; it does not read INA219 hardware or perform
 * ESP-NOW/MQTT communication itself.
 *
 * Overlays here are never persisted to NVS — end() (and the session
 * simply never having started) leaves zero trace.
 */

#ifndef KILOWATTS_DEVELOPMENT_SESSION_H
#define KILOWATTS_DEVELOPMENT_SESSION_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kilowatts {


/**
 * A Node's runtime Operating Environment — deliberately never the same
 * enum/state as LoadMode (Fixed/Auto): PRODUCTION vs DEVELOPMENT is a
 * system-wide concern, LoadMode is a per-Load configuration concern, and
 * this project keeps the two concepts and their state spaces independent.
 * Every Node boots PRODUCTION; DEVELOPMENT is only ever entered by an
 * explicit start() call at runtime, never inferred from missing hardware,
 * an uncommissioned state, or a compile-time flag.
 */
enum class OperatingEnvironment : std::uint8_t {
    PRODUCTION = 0U,
    DEVELOPMENT = 1U
};

const char* toText(OperatingEnvironment environment);


class DevelopmentSession {

public:

    DevelopmentSession();


    bool isActive() const;

    OperatingEnvironment getEnvironment() const;


    /**
     * PRODUCTION -> DEVELOPMENT. Rejected (returns false, no change) when
     * already DEVELOPMENT — a session must be explicitly end()ed before a
     * new one starts, so a caller can never silently stack sessions.
     */
    bool start();


    /**
     * DEVELOPMENT -> PRODUCTION, and discards every sensor override this
     * session armed — overlay state is never carried into the next
     * session or into production.
     *
     * Rejected (returns false, no change) when not currently active.
     */
    bool end();


    /**
     * Arms a simulated raw voltage/current override for the sensor
     * identified by i2cAddress. The caller is responsible for actually
     * applying it at the real measurement boundary (see
     * INA219Monitor::setDevelopmentOverride()) — this call only records
     * that policy decision.
     *
     * Rejected (returns false, no change) when no session is currently
     * active — simulation requires an active session first, an override
     * alone is never enough.
     */
    bool setSensorOverride(std::uint8_t i2cAddress, float voltageVolts, float currentAmps);


    /** Rejected (returns false, no change) when no session is active or no override exists for i2cAddress. */
    bool clearSensorOverride(std::uint8_t i2cAddress);


    /** Returns false when no override is currently armed for i2cAddress (including whenever no session is active, since end() clears every override). */
    bool findSensorOverride(std::uint8_t i2cAddress, float& voltageVolts, float& currentAmps) const;


    /** How many sensors currently have an armed override — for boot/diagnostic summaries. */
    std::size_t getNumberOfSensorOverrides() const;


private:

    struct SensorOverride {
        std::uint8_t i2cAddress;
        float voltageVolts;
        float currentAmps;
    };

    SensorOverride* findMutableOverride(std::uint8_t i2cAddress);
    const SensorOverride* findOverride(std::uint8_t i2cAddress) const;

    OperatingEnvironment environment_;
    std::vector<SensorOverride> overrides_;
};


} // namespace kilowatts

#endif // KILOWATTS_DEVELOPMENT_SESSION_H
