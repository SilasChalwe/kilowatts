#ifndef KILOWATTS_SMART_NODE_CONFIG_H
#define KILOWATTS_SMART_NODE_CONFIG_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace kilowatts {
namespace SmartNodeConfig {


constexpr std::array<std::uint8_t, 8U> VERIFIED_RELAY_GPIO_PINS{4U, 5U, 6U, 7U, 15U, 16U, 17U, 18U};

inline std::size_t getVerifiedRelayPinCount(){return VERIFIED_RELAY_GPIO_PINS.size();}

inline std::uint8_t getVerifiedRelayPin(std::size_t index){return index < VERIFIED_RELAY_GPIO_PINS.size() ? VERIFIED_RELAY_GPIO_PINS[index] : 0U;}

inline bool isVerifiedRelayPin(std::uint8_t pin)
{
    for (const std::uint8_t candidate : VERIFIED_RELAY_GPIO_PINS) {
        if (candidate == pin) {
            return true;
        }
    }
    return false;
}


} // namespace SmartNodeConfig
} // namespace kilowatts

#endif // KILOWATTS_SMART_NODE_CONFIG_H
