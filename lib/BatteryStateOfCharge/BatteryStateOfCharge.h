/**
 * @file BatteryStateOfCharge.h
 * @brief Declares the coulomb-counting battery State of Charge estimator
 *        (Section 4.6.2.2, Equation 4.8).
 *
 * BatteryStateOfCharge's one responsibility: maintain the battery State of
 * Charge (SoC) estimate by coulomb counting from the central INA219's
 * battery-bus current measurement, and persist it across reboots/power
 * interruptions so the estimate does not restart from an assumed value
 * every boot.
 *
 * SoC(t) = clamp[ SoC(t-Dt) - 100 * I_B(t) * Dt / (3600 * C_B), 0, 100 ]   (4.8)
 *
 * where I_B(t) is the battery current in amperes with discharge defined
 * as positive (so charging, a negative I_B, increases the SoC estimate),
 * Dt is the elapsed measurement interval in seconds, and C_B is the
 * configured battery capacity in ampere-hours. The 100/3600 factors
 * convert an amp-second charge delta into a percentage of C_B expressed
 * in amp-hours.
 *
 * This class does not read INA219 hardware itself (see INA219Monitor —
 * the caller supplies the already-measured/filtered battery current and
 * the elapsed interval), does not calculate the safe power budget
 * (PowerBudgetCalculator), does not know SoC_min/SoC_warn policy
 * thresholds (those belong to the caller/BestFirstSearch), and does not
 * run Best-First Search.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
 */

#ifndef KILOWATTS_BATTERY_STATE_OF_CHARGE_H
#define KILOWATTS_BATTERY_STATE_OF_CHARGE_H

#include <cstdint>

namespace kilowatts {


/**
 * Where the current SoC estimate actually came from — never collapsed
 * into the bare percentage itself, since a caller/log/MQTT payload must
 * never present a defaulted or never-established estimate as if it were
 * a genuine measurement (Section "SoC Must Have Validity").
 *
 *   UNKNOWN               — this object has never been initialize()d (for
 *                            example: true factory state, or no battery
 *                            sensor has ever been commissioned on this
 *                            Node, so nothing ever called initialize()).
 *   PERSISTED              — restored from a genuinely persisted NVS record.
 *   INITIAL_COMMISSIONING — no persisted record existed; initialize()'s own
 *                            configured starting estimate was used instead.
 *   COULOMB_COUNTING       — update() has actually advanced the estimate
 *                            from real measured battery current at least
 *                            once this boot, on top of either of the above.
 */
enum class StateOfChargeSource : std::uint8_t {
    UNKNOWN = 0U,
    PERSISTED = 1U,
    INITIAL_COMMISSIONING = 2U,
    COULOMB_COUNTING = 3U
};

const char* toText(StateOfChargeSource source);


class BatteryStateOfCharge {

public:

    BatteryStateOfCharge();


    /**
     * Configures the battery's rated capacity C_B (ampere-hours) and
     * establishes the starting SoC estimate. On the ESP32 target, a
     * valid persisted SoC from a previous run (see persist()) takes
     * precedence over defaultStateOfChargePercent, since "the value
     * stored in non-volatile memory provides the initial estimate after
     * restart" (Section 4.6.2.2) — getSource() reports
     * StateOfChargeSource::PERSISTED in that case. Otherwise
     * defaultStateOfChargePercent (a real, deliberate configured starting
     * assumption, not a fabricated reading) is used, and getSource()
     * reports StateOfChargeSource::INITIAL_COMMISSIONING. A host build
     * always takes the latter path, since it has no persistent storage.
     *
     * The caller decides *when* to call this at all: a Node with no
     * battery sensor commissioned should never call it, so
     * getStateOfChargePercent()/isValid() honestly report UNKNOWN/invalid
     * rather than presenting an unbacked default as real (Section "Do Not
     * Represent Absence Of An Initial SoC Estimate As A Genuine 0%").
     *
     * Rejected (returns false, this object stays uninitialized/UNKNOWN)
     * when batteryCapacityAmpHours is not a finite positive value, or
     * defaultStateOfChargePercent is outside [0, 100].
     */
    bool initialize(float batteryCapacityAmpHours, float defaultStateOfChargePercent);


    /** Returns true once initialize() has succeeded. */
    bool isInitialized() const;


    /** Equivalent to getSource() != StateOfChargeSource::UNKNOWN — whether getStateOfChargePercent() currently reflects a real estimate. */
    bool isValid() const;

    /** StateOfChargeSource::UNKNOWN until initialize() first succeeds. */
    StateOfChargeSource getSource() const;


    /**
     * Advances the SoC estimate by coulomb counting (Equation 4.8) using
     * the measured battery-bus current and the elapsed interval since
     * the previous update. On success, getSource() becomes
     * StateOfChargeSource::COULOMB_COUNTING.
     *
     * Rejected (returns false, the SoC estimate/source left unchanged)
     * when this object is not initialized, batteryCurrentAmps is not
     * finite, or deltaTimeSeconds is not a finite positive value.
     */
    bool update(float batteryCurrentAmps, float deltaTimeSeconds);


    /**
     * Returns the current SoC estimate, a percentage in [0, 100]. Always
     * check isValid() first — an invalid (UNKNOWN-source) estimate is
     * still returned as a real-looking float (0.0, this object's
     * constructed default) purely so callers have a well-defined value to
     * format, never as a claim that the battery is genuinely at 0%.
     */
    float getStateOfChargePercent() const;


    /**
     * Persists the current SoC estimate to NVS (ESP32 target only) so
     * the next boot's initialize() call can resume from it rather than
     * defaultStateOfChargePercent. Intended to be called periodically
     * (for example once per Optimisation Task cycle) and is deliberately
     * not called automatically by every update(), so the caller controls
     * how often flash is written.
     *
     * Returns false on a host build, or when the underlying NVS write
     * failed; the in-memory estimate is unaffected either way.
     */
    bool persist() const;


private:

    bool loadPersistedStateOfChargePercent(float& stateOfChargePercent) const;

    bool persistStateOfChargePercent(float stateOfChargePercent) const;

    static float clampPercent(float value);


    bool initialized_;
    float batteryCapacityAmpHours_;
    float stateOfChargePercent_;
    StateOfChargeSource source_;
};


} // namespace kilowatts

#endif // KILOWATTS_BATTERY_STATE_OF_CHARGE_H
