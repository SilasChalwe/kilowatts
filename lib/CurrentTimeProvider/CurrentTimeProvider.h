/**
 * @file CurrentTimeProvider.h
 * @brief Declares dual-mode (Automatic NTP / Manual) real wall-clock time
 *        for the Kilowatts firmware.
 *
 * CurrentTimeProvider has one responsibility: obtain, maintain and expose
 * valid real current local date/time for the rest of the firmware, from
 * exactly one of two legitimate production sources the user selects:
 *
 * - AUTOMATIC: ESP-IDF's real SNTP client synchronizes the ESP32 system
 *   clock from a real NTP server whenever internet/IP connectivity is
 *   available.
 * - MANUAL: the user/application supplies a real current local date/time
 *   directly, which becomes authoritative even while internet
 *   connectivity exists — this is not a fake test clock, it is a real
 *   production fallback for when internet time is unavailable or the
 *   user deliberately wants manual control.
 *
 * Both modes ultimately set the same ESP32 system clock — there are never
 * two independent clocks. Once a mode has actually established a valid
 * time (a real NTP synchronization, or a validated manual entry this
 * boot), every subsequent read comes from that one system clock, not a
 * fresh internet request and not a second software counter.
 *
 * Three concepts are tracked separately and must never be collapsed into
 * one flag:
 * - Time Mode: what the user selected (AUTOMATIC or MANUAL) — persisted
 *   in NVS so a reboot remembers it.
 * - Time Source: what actually established the currently valid clock
 *   right now (NONE, NTP or MANUAL).
 * - Time Validity: derived from Time Source — valid exactly when Source
 *   is not NONE.
 *
 * Switching Time Mode never lets the previous mode's source silently keep
 * asserting validity: changing mode always resets Source to NONE until
 * the newly selected mode's own mechanism (a real SNTP sync, or a fresh
 * setManualCurrentDateTime() call) re-establishes it. Selecting MANUAL
 * stops SNTP so it cannot silently overwrite a manually supplied time;
 * selecting AUTOMATIC resumes it, but Source only becomes NTP once a
 * synchronization has actually completed — never claimed in advance.
 *
 * A timestamp stored in flash (NVS) does not advance while the ESP32 is
 * completely powered off. The last manually entered date/time may be kept
 * in NVS as a recovery/reference value (getLastConfiguredManualDateTime()),
 * but it is never treated as current valid time by itself — only a real
 * NTP sync or a fresh manual entry this boot can make current time valid.
 * No external RTC is added in this phase.
 *
 * CurrentTimeProvider does not know about Loads, schedules, priorities,
 * power, ESP-NOW, or relays. It does not implement the NTP protocol
 * itself — that is ESP-IDF's SNTP client (see CurrentTimeProvider.cpp).
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
 */

#ifndef KILOWATTS_CURRENT_TIME_PROVIDER_H
#define KILOWATTS_CURRENT_TIME_PROVIDER_H

#include <atomic>
#include <cstdint>
#include <ctime>

struct timeval;

namespace kilowatts {


/**
 * What the user selected as the current time-keeping mechanism.
 *
 * A strongly typed mode instead of a manualMode bool so the operating
 * mode is self-explanatory at every call site.
 */
enum class TimeMode : std::uint8_t {
    AUTOMATIC = 0U,
    MANUAL = 1U
};


/**
 * What actually established the currently valid clock value, distinct
 * from TimeMode (what the user selected). NONE means current time is not
 * valid right now.
 */
enum class TimeSource : std::uint8_t {
    NONE = 0U,
    NTP = 1U,
    MANUAL = 2U
};


/**
 * A complete real wall-clock date/time supplied by the user for Manual
 * mode. One descriptive structure rather than an unexplained parameter
 * list — year is required (not just hour/minute) so the clock is correct
 * across a reboot or a date rollover, not only within "today".
 *
 * Interpreted as local time (this Node's configured timezone), not UTC.
 */
struct ManualDateTime {
    std::uint16_t year;
    std::uint8_t month;
    std::uint8_t day;
    std::uint8_t hour;
    std::uint8_t minute;
    std::uint8_t second;
};


class CurrentTimeProvider {

public:

    CurrentTimeProvider();

    ~CurrentTimeProvider();

    /*
     * The SNTP synchronization callback is static and routes back to
     * exactly one instance (see instance_ below), so CurrentTimeProvider
     * is not copyable — the same assumption EspNowCommunication makes for
     * its own static ESP-NOW callbacks.
     */
    CurrentTimeProvider(const CurrentTimeProvider&) = delete;
    CurrentTimeProvider& operator=(const CurrentTimeProvider&) = delete;


    /**
     * Configures the local timezone, loads the persisted Time Mode from
     * NVS (defaulting to AUTOMATIC the first time this Node ever runs),
     * and — only when that mode is AUTOMATIC — starts ESP-IDF's SNTP
     * client. When the persisted mode is MANUAL, SNTP is left stopped and
     * current time stays invalid until setManualCurrentDateTime() is
     * called this boot; the mode selection persisting does not imply the
     * previous time value survived the reboot.
     *
     * Call this once during startup. Calling it again after it already
     * succeeded is a no-op that returns true.
     *
     * Returns false only when the underlying setup itself failed (NVS,
     * the network interface, or SNTP initialisation). This is independent
     * of whether a synchronization has actually completed — see
     * isCurrentTimeValid() for that.
     */
    bool initializeTimeSynchronization();


    /** Returns the Time Mode currently selected (persisted across reboots). */
    TimeMode getTimeMode() const;


    /**
     * Changes the selected Time Mode and persists it to NVS.
     *
     * A change always resets Time Source to NONE (current time becomes
     * invalid) until the newly selected mode's own mechanism
     * re-establishes it — a stale source from the previous mode is never
     * carried over. Calling this with the mode already selected is a
     * no-op that returns true and does not reset Source.
     *
     * Switching to MANUAL stops SNTP (see the module README for the
     * exact ESP-IDF call) so an in-flight or future synchronization
     * cannot silently overwrite a manually supplied time.
     *
     * Switching to AUTOMATIC resumes SNTP; Source only becomes NTP once a
     * real synchronization actually completes afterward.
     *
     * Returns false when persisting to NVS, or (re)starting/stopping
     * SNTP, failed. The in-memory mode is still updated either way.
     */
    bool setTimeMode(TimeMode timeMode);


    /**
     * Validates manualDateTime and, if every field is in range, adopts it
     * as the ESP32 system clock's current local time and marks Time
     * Source as MANUAL.
     *
     * Rejected (returns false, system clock and Time Source unchanged)
     * when any field is out of range — an impossible value (for example
     * month 13, or 30 February) is never silently normalized into a
     * nearby valid date. See isManualDateTimeValid() for that check on
     * its own.
     *
     * Does not itself change Time Mode: call setTimeMode(TimeMode::MANUAL)
     * too when Automatic is currently selected, so SNTP cannot later
     * overwrite this value.
     */
    bool setManualCurrentDateTime(const ManualDateTime& manualDateTime);


    /**
     * Returns true when every field of manualDateTime is a real,
     * in-range date/time (correct days-per-month, including leap years)
     * — the same check setManualCurrentDateTime() applies before ever
     * touching the system clock. Pure validation, no hardware access, so
     * it is exercised directly by this module's host test.
     */
    static bool isManualDateTimeValid(const ManualDateTime& manualDateTime);


    /** What actually established the current clock value right now. */
    TimeSource getCurrentTimeSource() const;


    /** Equivalent to getCurrentTimeSource() != TimeSource::NONE. */
    bool isCurrentTimeValid() const;


    /**
     * Reads current local date/time from the ESP32 system clock.
     *
     * Returns false, and leaves localDateTime unchanged, when
     * isCurrentTimeValid() is false.
     */
    bool getCurrentLocalDateTime(std::tm& localDateTime) const;


    /** Convenience accessors built on getCurrentLocalDateTime(). */
    bool getCurrentLocalHour(std::uint8_t& hour) const;

    bool getCurrentLocalMinute(std::uint8_t& minute) const;

    bool getCurrentLocalSecond(std::uint8_t& second) const;


    /**
     * Returns the last manually entered date/time ever persisted to NVS
     * on this device, purely as a recovery/reference value — this is
     * NEVER a claim that current time is valid. A flash-stored timestamp
     * does not advance while the ESP32 is completely powered off, so it
     * must not be, and is not, treated as current time by this method or
     * by isCurrentTimeValid()/getCurrentLocalDateTime().
     *
     * Returns false when no manual time has ever been persisted.
     */
    bool getLastConfiguredManualDateTime(std::tm& lastManualDateTime) const;


    /**
     * Logs Time Mode, Time Source, current validity, and — only when
     * valid — current local time, current UTC time, local hour and local
     * minute. Also reports SNTP status while in AUTOMATIC mode. Intended
     * as the on-device diagnostic path for verifying real hardware time
     * behaviour; see the module README for how to use it.
     */
    void printDiagnosticReport() const;


private:

    static void handleTimeSynchronizationEvent(struct timeval* synchronizedTime);

    bool initializeNonVolatileStorage() const;

    TimeMode loadPersistedTimeMode() const;

    bool persistTimeMode(TimeMode timeMode) const;

    bool persistLastManualEpochSeconds(std::int64_t epochSeconds) const;


    /** Starts (or restarts) ESP-IDF's SNTP client. */
    bool startAutomaticSynchronization();

    /** Stops ESP-IDF's SNTP client if it is currently running. */
    void stopAutomaticSynchronization();


    bool initialized_;

    /** True while ESP-IDF's SNTP client is actually running. */
    bool sntpActive_;

    std::atomic<TimeMode> timeMode_;
    std::atomic<TimeSource> timeSource_;

    static CurrentTimeProvider* instance_;
};


} // namespace kilowatts

#endif // KILOWATTS_CURRENT_TIME_PROVIDER_H
