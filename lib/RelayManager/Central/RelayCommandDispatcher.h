/**
 * @file RelayCommandDispatcher.h
 * @brief Declares OFF-before-ON relay dispatch ordering and
 *        command/acknowledgement tracking.
 *
 * After Best-First Search produces a target logical schedule, Loads
 * transitioning ON -> OFF must be commanded first (releasing capacity)
 * before any OFF -> ON transition is sent, and Central tracks each
 * dispatched remote command until it is acknowledged or fails. The
 * acknowledgement reports command-processing/GPIO-write success only;
 * it is not downstream-device feedback.
 *
 * RelayCommandDispatcher owns exactly two things:
 *
 * 1. buildDispatchOrder() — a pure function that takes every target the
 *    current planning cycle wants commanded and returns them in dispatch
 *    order: every OFF transition first (any order), then every ON
 *    transition afterward in the same relative order they were supplied
 *    (Best-First admission order is preserved through BestFirstSearch's
 *    sequential allocation). There is no stored "last confirmed state" to
 *    compare against — Load does not model physical/GPIO state, and this
 *    firmware never assumes a relay already sits at its desired state, so
 *    every target the caller supplies is dispatched.
 * 2. Command/acknowledgement bookkeeping — commandId allocation and
 *    pending tracking for whichever RelayCommandPacket the caller actually
 *    transmits (over ESP-NOW for a remote Smart Node, or directly through
 *    RelayController for Central's own local relays). completeCommand()
 *    records only whether the acknowledgement (or the caller, on timeout)
 *    reported success — never a physical read-back, which this class has
 *    no way to know. Timeout detection is the caller's responsibility (see
 *    dispatchRelayTargetAndWait() in src/central/namespace.h): it waits on
 *    each dispatched command against its own deadline, which doubles as
 *    the live per-target power-safety recheck window between successive
 *    ON dispatches, so this class does not run a separate/competing
 *    expiry sweep of its own.
 *
 * It does not send ESP-NOW messages itself (EspNowCommunication), does
 * not actuate GPIOs itself (RelayController), does not run Best-First
 * Search, and does not decide *what* the target schedule is — only how a
 * given schedule is sequenced and how in-flight commands are tracked.
 */

#ifndef KILOWATTS_RELAY_COMMAND_DISPATCHER_H
#define KILOWATTS_RELAY_COMMAND_DISPATCHER_H

#include "Load.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kilowatts {


class RelayCommandDispatcher {

public:

    /**
     * One relay this planning cycle wants commanded to desiredOn.
     * nodeMacAddress and relayPin together are exactly this Load's
     * `Load::Id`.
     */
    struct RelayTarget {
        Load::MacAddress nodeMacAddress;
        std::uint8_t relayPin;
        bool desiredOn;
    };


    /**
     * One command Central has dispatched (or is about to dispatch) and is
     * waiting to see completed/acknowledged, tracked by the commandId carried in the
     * matching RelayCommandPacket/RelayCommandAcknowledgementPacket.
     */
    struct PendingCommand {
        std::uint32_t commandId;
        Load::MacAddress nodeMacAddress;
        std::uint8_t relayPin;
        bool desiredOn;
    };


    /**
     * Builds the OFF-before-ON dispatch order from every candidate
     * target. Preserves the caller's relative ordering within each phase
     * — passing targets in Best-First admission order (the order
     * BestFirstSearch::isLoadSelectedToBeOn() was evaluated in) keeps
     * that same priority order within the ON phase.
     *
     * Every supplied target is dispatched — there is no stored previous
     * hardware state to compare against, so no target is ever dropped
     * as a no-op.
     *
     * Always compiled and hardware-free: no ESP-NOW/RelayController
     * dependency, so directly host-testable.
     */
    static std::vector<RelayTarget> buildDispatchOrder(const std::vector<RelayTarget>& targets);


    RelayCommandDispatcher();


    /**
     * Allocates a new commandId, records target as pending, and returns
     * the allocated commandId. commandIds are unique for the lifetime of
     * this object (monotonically increasing), so a late acknowledgement
     * for a superseded command can never be confused with the current one.
     */
    std::uint32_t beginCommand(const RelayTarget& target);


    /** Returns true while commandId has been begun but not yet completed/expired. */
    bool isPending(std::uint32_t commandId) const;


    /** Returns the pending record for commandId, or nullptr when it is not pending. */
    const PendingCommand* findPendingCommand(std::uint32_t commandId) const;


    /**
     * Removes commandId from the pending set, recording success exactly
     * as the caller reports it — an acknowledgement's own GPIO-command
     * success flag,
     * or false on caller-detected timeout. This class never infers a
     * physical relay state itself; it only remembers what it was told.
     *
     * Returns false when commandId was not pending.
     */
    bool completeCommand(std::uint32_t commandId, bool success);


    /**
     * Returns whether commandId was the most recently completed command
     * and completeCommand() recorded it as successful. Returns false for
     * any commandId other than the one most recently completed.
     */
    bool wasCommandSuccessful(std::uint32_t commandId) const;


    /** Returns how many commands are currently pending. */
    std::size_t getNumberOfPendingCommands() const;


    /** Clears every pending command without resetting the commandId counter. */
    void clearPendingCommands();


private:

    std::vector<PendingCommand> pendingCommands_;
    std::uint32_t nextCommandId_;
    std::uint32_t lastCompletedCommandId_;
    bool lastCompletedCommandSuccess_;
};


} // namespace kilowatts

#endif // KILOWATTS_RELAY_COMMAND_DISPATCHER_H
