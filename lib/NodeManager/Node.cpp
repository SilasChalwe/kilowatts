#include "Node.h"

namespace kilowatts {


Node::Node(const MacAddress& macAddress)
    : macAddress_(macAddress)
{
}


bool Node::addLoad(const Load& load)
{
    if (load.getMacAddress() != macAddress_) {
        return false;
    }

    if (isRelayPinAlreadyUsed(load.getRelayPin())) {
        return false;
    }

    loads_.push_back(load);
    return true;
}


const Node::MacAddress& Node::getMacAddress() const
{
    return macAddress_;
}


std::size_t Node::getNumberOfLoads() const
{
    return loads_.size();
}


Load* Node::getLoad(std::size_t loadIndex)
{
    if (loadIndex >= loads_.size()) {
        return nullptr;
    }

    return &loads_[loadIndex];
}


const Load* Node::getLoad(std::size_t loadIndex) const
{
    if (loadIndex >= loads_.size()) {
        return nullptr;
    }

    return &loads_[loadIndex];
}


Load* Node::getLoadByRelayPin(std::uint8_t relayPin)
{
    for (Load& load : loads_) {
        if (load.getRelayPin() == relayPin) {
            return &load;
        }
    }

    return nullptr;
}


const Load* Node::getLoadByRelayPin(std::uint8_t relayPin) const
{
    for (const Load& load : loads_) {
        if (load.getRelayPin() == relayPin) {
            return &load;
        }
    }

    return nullptr;
}


bool Node::removeLoadByRelayPin(std::uint8_t relayPin)
{
    for (std::size_t i = 0U; i < loads_.size(); ++i) {
        if (loads_[i].getRelayPin() == relayPin) {
            loads_.erase(loads_.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }

    return false;
}


bool Node::isRelayPinAlreadyUsed(std::uint8_t relayPin) const
{
    return getLoadByRelayPin(relayPin) != nullptr;
}


} // namespace kilowatts