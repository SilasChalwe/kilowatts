/**
 * @file DevelopmentSession.cpp
 * @brief Implements the explicit, runtime-only Development Session a
 *        Node's Operating Environment can enter.
 *
 * Entirely plain, hardware-free/network-free C++ — no ESP_PLATFORM split
 * is needed, so it is directly host-testable (see
 * test/DevelopmentSession/).
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 14 August 2026
 */

#include "DevelopmentSession.h"

namespace kilowatts {


const char* toText(OperatingEnvironment environment)
{
    switch (environment) {
        case OperatingEnvironment::PRODUCTION: return "PRODUCTION";
        case OperatingEnvironment::DEVELOPMENT: return "DEVELOPMENT";
    }

    return "UNKNOWN";
}


DevelopmentSession::DevelopmentSession()
    : environment_(OperatingEnvironment::PRODUCTION),
      overrides_()
{
}


bool DevelopmentSession::isActive() const
{
    return environment_ == OperatingEnvironment::DEVELOPMENT;
}


OperatingEnvironment DevelopmentSession::getEnvironment() const
{
    return environment_;
}


bool DevelopmentSession::start()
{
    if (isActive()) {
        return false;
    }

    environment_ = OperatingEnvironment::DEVELOPMENT;
    return true;
}


bool DevelopmentSession::end()
{
    if (!isActive()) {
        return false;
    }

    environment_ = OperatingEnvironment::PRODUCTION;
    overrides_.clear();
    return true;
}


DevelopmentSession::SensorOverride* DevelopmentSession::findMutableOverride(std::uint8_t i2cAddress)
{
    for (SensorOverride& override : overrides_) {
        if (override.i2cAddress == i2cAddress) {
            return &override;
        }
    }

    return nullptr;
}


const DevelopmentSession::SensorOverride* DevelopmentSession::findOverride(std::uint8_t i2cAddress) const
{
    for (const SensorOverride& override : overrides_) {
        if (override.i2cAddress == i2cAddress) {
            return &override;
        }
    }

    return nullptr;
}


bool DevelopmentSession::setSensorOverride(std::uint8_t i2cAddress, float voltageVolts, float currentAmps)
{
    if (!isActive()) {
        return false;
    }

    SensorOverride* existing = findMutableOverride(i2cAddress);
    if (existing != nullptr) {
        existing->voltageVolts = voltageVolts;
        existing->currentAmps = currentAmps;
        return true;
    }

    overrides_.push_back(SensorOverride{i2cAddress, voltageVolts, currentAmps});
    return true;
}


bool DevelopmentSession::clearSensorOverride(std::uint8_t i2cAddress)
{
    if (!isActive()) {
        return false;
    }

    for (std::size_t i = 0U; i < overrides_.size(); ++i) {
        if (overrides_[i].i2cAddress == i2cAddress) {
            overrides_.erase(overrides_.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }

    return false;
}


bool DevelopmentSession::findSensorOverride(std::uint8_t i2cAddress, float& voltageVolts, float& currentAmps) const
{
    if (!isActive()) {
        return false;
    }

    const SensorOverride* override = findOverride(i2cAddress);
    if (override == nullptr) {
        return false;
    }

    voltageVolts = override->voltageVolts;
    currentAmps = override->currentAmps;
    return true;
}


std::size_t DevelopmentSession::getNumberOfSensorOverrides() const
{
    return overrides_.size();
}


} // namespace kilowatts
