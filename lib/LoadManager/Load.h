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


enum class LoadPowerType : std::uint8_t {
    AC = 0U,
    DC = 1U
};


/**
 * Preferred running window for an AUTO Load.
 *
 * Example:
 * AutoSchedule{true, 6, 0, 8, 0} means 06:00 up to 08:00.
 * AutoSchedule{true, 22, 0, 2, 0} means 22:00 up to 02:00 next day.
 *
 * The three-argument constructor is retained for compatibility with older
 * start-only call sites and creates a one-hour window. New configuration
 * paths should always supply both start and end times explicitly.
 */
struct AutoSchedule {
    bool enabled;

    union {
        std::uint8_t startHour;
        std::uint8_t hour; // Legacy alias for startHour.
    };

    union {
        std::uint8_t startMinute;
        std::uint8_t minute; // Legacy alias for startMinute.
    };

    std::uint8_t endHour;
    std::uint8_t endMinute;

    AutoSchedule()
        : enabled(false),
          startHour(0U),
          startMinute(0U),
          endHour(0U),
          endMinute(0U)
    {
    }

    AutoSchedule(
        bool scheduleEnabled,
        std::uint8_t scheduleStartHour,
        std::uint8_t scheduleStartMinute,
        std::uint8_t scheduleEndHour,
        std::uint8_t scheduleEndMinute)
        : enabled(scheduleEnabled),
          startHour(scheduleEnabled ? scheduleStartHour : 0U),
          startMinute(scheduleEnabled ? scheduleStartMinute : 0U),
          endHour(scheduleEnabled ? scheduleEndHour : 0U),
          endMinute(scheduleEnabled ? scheduleEndMinute : 0U)
    {
    }

    AutoSchedule(
        bool scheduleEnabled,
        std::uint8_t scheduleStartHour,
        std::uint8_t scheduleStartMinute)
        : enabled(scheduleEnabled),
          startHour(scheduleEnabled ? scheduleStartHour : 0U),
          startMinute(scheduleEnabled ? scheduleStartMinute : 0U),
          endHour(scheduleEnabled
              ? static_cast<std::uint8_t>((scheduleStartHour + 1U) % 24U)
              : 0U),
          endMinute(scheduleEnabled ? scheduleStartMinute : 0U)
    {
    }
};


class Load {

public:

    using MacAddress = std::array<std::uint8_t, 6>;

    struct Id {
        MacAddress macAddress;
        std::uint8_t relayPin;
    };

    Load(
        const Id& id,
        const std::string& name,
        float powerRatingWatts,
        std::uint16_t priority,
        LoadPowerType powerType,
        LoadMode::Value mode
    );

    const Id& getId() const;
    const MacAddress& getMacAddress() const;
    std::uint8_t getRelayPin() const;

    void setName(const std::string& name);
    const std::string& getName() const;

    void setMode(LoadMode::Value mode);
    LoadMode::Value getMode() const;

    bool isFixed() const;
    bool isAuto() const;
    bool isOn() const;
    bool isOff() const;

    void setLastBestFirstRejectionReason(std::uint8_t reason);
    std::uint8_t getLastBestFirstRejectionReason() const;

    bool setPowerRatingWatts(float powerRatingWatts);
    float getPowerRatingWatts() const;

    void setPowerType(LoadPowerType powerType);
    LoadPowerType getPowerType() const;

    void setPriority(std::uint16_t priority);
    std::uint16_t getPriority() const;

    /**
     * Sets the preferred running window for an AUTO Load.
     *
     * Start/end hours must be 0 to 23 and minutes 0 to 59. Start and end
     * must differ. An end earlier than the start represents an overnight
     * window crossing midnight.
     */
    bool setSchedule(AutoSchedule schedule);

    AutoSchedule getSchedule() const;
    void clearSchedule();


private:

    Id id_;
    std::string name_;
    std::uint8_t lastBestFirstRejectionReason_;
    float powerRatingWatts_;
    LoadPowerType powerType_;
    std::uint16_t priority_;
    LoadMode::Value mode_;
    AutoSchedule schedule_;
};


void formatMacAddressText(char* buffer, std::size_t bufferSize, const Load::MacAddress& mac);


} // namespace kilowatts

#endif // KILOWATTS_LOAD_H
