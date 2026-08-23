#include "Load.h"

#include <cassert>
#include <cmath>
#include <cstdio>


namespace kilowatts {


Load::Load(
    const Id& id,
    const std::string& name,
    float powerRatingWatts,
    std::uint16_t priority,
    LoadPowerType powerType,
    LoadMode::Value mode
)
    : id_(id),
      name_(name),
      lastBestFirstRejectionReason_(0U),
      powerRatingWatts_(0.0F),
      powerType_(powerType),
      priority_(priority),
      mode_(mode),
      schedule_{false, 0U, 0U}
{
    [[maybe_unused]] const bool powerRatingValid =
        setPowerRatingWatts(powerRatingWatts);
    assert(powerRatingValid && "Load constructed with invalid power rating");
}


const Load::Id& Load::getId() const
{
    return id_;
}


const Load::MacAddress& Load::getMacAddress() const
{
    return id_.macAddress;
}


std::uint8_t Load::getRelayPin() const
{
    return id_.relayPin;
}


void Load::setName(const std::string& name)
{
    name_ = name;
}


const std::string& Load::getName() const
{
    return name_;
}


void Load::setMode(LoadMode::Value mode)
{
    mode_ = mode;

    if (isFixed()) {
        clearSchedule();
    }
}


LoadMode::Value Load::getMode() const
{
    return mode_;
}


bool Load::isFixed() const
{
    return mode_ == LoadMode::Fixed::ON ||
           mode_ == LoadMode::Fixed::OFF;
}


bool Load::isAuto() const
{
    return mode_ == LoadMode::Auto::ON ||
           mode_ == LoadMode::Auto::OFF;
}


bool Load::isOn() const
{
    return mode_ == LoadMode::Fixed::ON ||
           mode_ == LoadMode::Auto::ON;
}


bool Load::isOff() const
{
    return mode_ == LoadMode::Fixed::OFF ||
           mode_ == LoadMode::Auto::OFF;
}


void Load::setLastBestFirstRejectionReason(std::uint8_t reason)
{
    lastBestFirstRejectionReason_ = reason;
}


std::uint8_t Load::getLastBestFirstRejectionReason() const
{
    return lastBestFirstRejectionReason_;
}


bool Load::setPowerRatingWatts(float powerRatingWatts)
{
    if (!std::isfinite(powerRatingWatts) || powerRatingWatts < 0.0F) {
        return false;
    }

    powerRatingWatts_ = powerRatingWatts;
    return true;
}


float Load::getPowerRatingWatts() const
{
    return powerRatingWatts_;
}


void Load::setPowerType(LoadPowerType powerType)
{
    powerType_ = powerType;
}


LoadPowerType Load::getPowerType() const
{
    return powerType_;
}


void Load::setPriority(std::uint16_t priority)
{
    priority_ = priority;
}


std::uint16_t Load::getPriority() const
{
    return priority_;
}


bool Load::setSchedule(AutoSchedule schedule)
{
    if (!isAuto()) {
        return false;
    }

    if (!schedule.enabled) {
        clearSchedule();
        return true;
    }

    if (schedule.hour > 23U || schedule.minute > 59U) {
        return false;
    }

    schedule_ = schedule;
    return true;
}


AutoSchedule Load::getSchedule() const
{
    return schedule_;
}


void Load::clearSchedule()
{
    schedule_ = {false, 0U, 0U};
}


void formatMacAddressText(char* buffer, std::size_t bufferSize, const Load::MacAddress& mac)
{
    std::snprintf(buffer, bufferSize, "%02X:%02X:%02X:%02X:%02X:%02X",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}


} // namespace kilowatts