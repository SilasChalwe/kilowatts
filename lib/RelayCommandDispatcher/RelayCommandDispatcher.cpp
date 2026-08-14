/**
 * @file RelayCommandDispatcher.cpp
 * @brief Implements OFF-before-ON relay dispatch ordering and
 *        command/acknowledgement tracking (Section 4.6.4.3, Algorithm 4.6).
 *
 * Entirely plain, hardware-free/network-free C++ — no ESP_PLATFORM split
 * is needed, since this class never touches ESP-NOW or GPIO itself.
 * Always compiled and fully host-testable (see
 * test/RelayCommandDispatcher/).
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
 */

#include "RelayCommandDispatcher.h"

namespace kilowatts {


std::vector<RelayCommandDispatcher::RelayTarget> RelayCommandDispatcher::buildDispatchOrder(
    const std::vector<RelayTarget>& targets)
{
    std::vector<RelayTarget> offPhase;
    std::vector<RelayTarget> onPhase;

    for (const RelayTarget& target : targets) {
        if (target.currentlyConfirmedValid && target.desiredOn == target.currentlyConfirmedOn) {
            /*
             * Already at the desired state, and that confirmation can
             * actually be trusted; nothing to command. An INVALID
             * (unknown) confirmed state is never treated as a match —
             * dispatching the command is exactly how an unknown relay
             * gets re-established to a trusted state.
             */
            continue;
        }

        if (target.desiredOn) {
            onPhase.push_back(target);
        } else {
            offPhase.push_back(target);
        }
    }

    /*
     * OFF transitions are commanded first so capacity is released before
     * any new load is energised, then ON transitions follow in the
     * caller's original (Best-First admission) order.
     */
    std::vector<RelayTarget> dispatchOrder;
    dispatchOrder.reserve(offPhase.size() + onPhase.size());
    dispatchOrder.insert(dispatchOrder.end(), offPhase.begin(), offPhase.end());
    dispatchOrder.insert(dispatchOrder.end(), onPhase.begin(), onPhase.end());

    return dispatchOrder;
}


RelayCommandDispatcher::RelayCommandDispatcher()
    : pendingCommands_(),
      nextCommandId_(1U)
{
}


std::uint32_t RelayCommandDispatcher::beginCommand(const RelayTarget& target, std::uint32_t nowMilliseconds)
{
    const std::uint32_t commandId = nextCommandId_;
    ++nextCommandId_;

    pendingCommands_.push_back(PendingCommand{
        commandId,
        target.nodeMacAddress,
        target.relayPin,
        target.desiredOn,
        nowMilliseconds
    });

    return commandId;
}


bool RelayCommandDispatcher::isPending(std::uint32_t commandId) const
{
    return findPendingCommand(commandId) != nullptr;
}


const RelayCommandDispatcher::PendingCommand* RelayCommandDispatcher::findPendingCommand(std::uint32_t commandId) const
{
    for (const PendingCommand& pending : pendingCommands_) {
        if (pending.commandId == commandId) {
            return &pending;
        }
    }

    return nullptr;
}


bool RelayCommandDispatcher::completeCommand(std::uint32_t commandId)
{
    for (std::size_t i = 0U; i < pendingCommands_.size(); ++i) {
        if (pendingCommands_[i].commandId == commandId) {
            pendingCommands_.erase(pendingCommands_.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }

    return false;
}


std::vector<RelayCommandDispatcher::PendingCommand> RelayCommandDispatcher::findExpiredCommands(
    std::uint32_t nowMilliseconds, std::uint32_t timeoutMilliseconds) const
{
    std::vector<PendingCommand> expired;

    for (const PendingCommand& pending : pendingCommands_) {
        const std::uint32_t elapsedMilliseconds = nowMilliseconds - pending.dispatchedAtMilliseconds;
        if (elapsedMilliseconds > timeoutMilliseconds) {
            expired.push_back(pending);
        }
    }

    return expired;
}


std::size_t RelayCommandDispatcher::getNumberOfPendingCommands() const
{
    return pendingCommands_.size();
}


void RelayCommandDispatcher::clearPendingCommands()
{
    pendingCommands_.clear();
}


} // namespace kilowatts
