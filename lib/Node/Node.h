/**
 * @file Node.h
 * @brief Declares one ESP32 Node and the Loads that belong to it.
 *
 * A Node represents one ESP32 in the system.
 * The Node is globally identified by its ESP32 MAC address.
 *
 * Every Load stored by the Node must contain the same MAC address
 * and is uniquely identified by:
 *
 *     Node MAC address + Load relay pin
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 8 May 2026
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

    /**
     * Uses the same MAC address type used by Load.
     */
    using MacAddress = Load::MacAddress;


    /**
     * Creates one ESP32 Node identified by its MAC address.
     */
    explicit Node(const MacAddress& macAddress);


    /**
     * Adds one Load that belongs to this Node.
     *
     * The Load is rejected when:
     * - its MAC address does not match this Node's MAC address, or
     * - another Load in this Node already uses the same relay pin.
     */
    bool addLoad(const Load& load);


    /**
     * Returns this Node's MAC address.
     */
    const MacAddress& getMacAddress() const;


    /**
     * Returns the number of Loads belonging to this Node.
     */
    std::size_t getNumberOfLoads() const;


    /**
     * Returns the Load stored at the given position.
     *
     * Returns nullptr when the position does not exist.
     */
    Load* getLoad(std::size_t loadIndex);


    const Load* getLoad(std::size_t loadIndex) const;


    /**
     * Returns the Load connected to the given relay pin.
     *
     * Returns nullptr when this Node has no Load using that relay pin.
     */
    Load* getLoadByRelayPin(std::uint8_t relayPin);


    const Load* getLoadByRelayPin(std::uint8_t relayPin) const;


    /**
     * Removes the Load using the given relay pin, if this Node has one.
     *
     * Used to prune a Load that a Node's own later report no longer
     * includes (see CentralNodeRegistry::applyNodeReport()) - a Node's
     * real Load count can legitimately shrink (for example after
     * decommissioning), and this registry must not keep reporting a Load
     * that no longer physically exists.
     *
     * Returns true when a Load was actually removed.
     */
    bool removeLoadByRelayPin(std::uint8_t relayPin);


private:

    /**
     * Checks whether another Load in this Node
     * already uses the given relay pin.
     */
    bool isRelayPinAlreadyUsed(std::uint8_t relayPin) const;


    /**
     * Global MAC address identifying this ESP32 Node.
     */
    MacAddress macAddress_;


    /**
     * Loads that physically belong to this ESP32 Node.
     */
    std::vector<Load> loads_;
};


} // namespace kilowatts

#endif // KILOWATTS_NODE_H