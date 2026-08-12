/**
 * @file Load.cpp
 * @brief Implements one electrical load connected to a Central Node or Smart Node.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 8 May 2026
 */


#include "Load.h"


#include <cmath>


namespace kilowatts {


Load::Load(
    const Id& id,
    const std::string& name,
    LoadPower power,
    std::uint16_t priority,
    LoadMode::Value mode
)
    : id_(id),
      name_(name),
      measurements_{0.0F, 0.0F, 0.0F},
      power_{0.0F, 0.0F},
      priority_(priority),
      mode_(mode),
      schedule_{false, 0U, 0U}
{
    setPower(power);
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


bool Load::setMeasurements(LoadMeasurements measurements)
{
    if (!std::isfinite(measurements.voltageVolts) ||
        !std::isfinite(measurements.currentAmps) ||
        !std::isfinite(measurements.powerWatts) ||
        measurements.voltageVolts < 0.0F ||
        measurements.currentAmps < 0.0F ||
        measurements.powerWatts < 0.0F) {
        return false;
    }

    measurements_ = measurements;
    return true;
}


LoadMeasurements Load::getMeasurements() const
{
    return measurements_;
}


bool Load::setPower(LoadPower power)
{
    if (!std::isfinite(power.runningWatts) ||
        !std::isfinite(power.startupWatts) ||
        power.runningWatts < 0.0F ||
        power.startupWatts < power.runningWatts) {
        return false;
    }

    power_ = power;
    return true;
}


LoadPower Load::getPower() const
{
    return power_;
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

    if (schedule.hour > 23U ||
        schedule.minute > 59U) {
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


} // namespace kilowatts