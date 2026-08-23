/**
 * @file Node.h
 * @brief One ESP32 Node and the Loads that belong to it.
 *
 * A Load is identified within a Node by Node MAC address + Load relay pin.
 */

#ifndef KILOWATTS_NODE_H
#define KILOWATTS_NODE_H

#include "Load.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kilowatts {


class Node {

public:

    using MacAddress = Load::MacAddress;


    explicit Node(const MacAddress& macAddress);


    /**
     * Adds one Load that belongs to this Node.
     *
     * The Load is rejected when:
     * - its MAC address does not match this Node's MAC address, or
     * - another Load in this Node already uses the same relay pin.
     */
    bool addLoad(const Load& load);


    const MacAddress& getMacAddress() const;


    std::size_t getNumberOfLoads() const;


    /** Returns nullptr when the position does not exist. */
    Load* getLoad(std::size_t loadIndex);


    const Load* getLoad(std::size_t loadIndex) const;


    /** Returns nullptr when this Node has no Load using that relay pin. */
    Load* getLoadByRelayPin(std::uint8_t relayPin);


    const Load* getLoadByRelayPin(std::uint8_t relayPin) const;


    /** Returns true when a Load was actually removed. */
    bool removeLoadByRelayPin(std::uint8_t relayPin);


private:

    bool isRelayPinAlreadyUsed(std::uint8_t relayPin) const;


    MacAddress macAddress_;


    std::vector<Load> loads_;
};


} // namespace kilowatts

#endif // KILOWATTS_NODE_H