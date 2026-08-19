/**
 * @file RelayCommandDispatcher.h
 * @brief Declares OFF-before-ON relay dispatch ordering and
 *        command/acknowledgement tracking.
 *
 * After Best-First Search produces a target logical schedule, Loads
 * transitioning ON -> OFF must be commanded first (releasing capacity)
 * before any OFF -> ON transition is sent, and Central tracks each
 * dispatched command until it is confirmed or fails, rather than
 * assuming the desired state is the confirmed physical state.
 *
 * RelayCommandDispatcher owns exactly two things:
 *
 * 1. buildDispatchOrder() — a pure function that takes every Load whose
 *    target state differs from RelayController's last GPIO read-back
 *    confirmed state, and returns them in dispatch order: every OFF
 *    transition first (any order), then every ON transition afterward in
 *    the same relative order they were supplied (Best-First admission
 *    order is preserved through BestFirstSearch's sequential allocation).
 *    A target already at its desired confirmed state is dropped — there
 *    is nothing to command.
 * 2. Command/acknowledgement bookkeeping — commandId allocation, pending
 *    tracking, and timeout detection for whichever RelayCommandPacket the
 *    caller actually transmits (over ESP-NOW for a remote Smart Node, or
 *    directly through RelayController for Central's own local relays).
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
     * One relay whose target state may differ from its last confirmed
     * state. nodeMacAddress/relayPin together are exactly this Load's
     * owning Branch identity (BestFirstSearch::BranchId).
     */
    struct RelayTarget {
        Load::MacAddress nodeMacAddress;
        std::uint8_t relayPin;
        bool desiredOn;
        bool currentlyConfirmedOn;

        /**
         * Whether currentlyConfirmedOn reflects a trustworthy hardware
         * confirmation (Load::isConfirmedRelayStateValid()). When false,
         * buildDispatchOrder() never treats this target as a no-op even
         * if currentlyConfirmedOn already equals desiredOn — an unknown
         * physical state must never be assumed to already match what is
         * wanted, since a command is exactly how a genuinely unknown
         * relay gets re-established to a trusted state.
         */
        bool currentlyConfirmedValid;
    };


    /**
     * One command Central has dispatched (or is about to dispatch) and is
     * waiting to see confirmed, tracked by the commandId carried in the
     * matching RelayCommandPacket/RelayCommandAcknowledgementPacket.
     */
    struct PendingCommand {
        std::uint32_t commandId;
        Load::MacAddress nodeMacAddress;
        std::uint8_t relayPin;
        bool desiredOn;
        std::uint32_t dispatchedAtMilliseconds;
    };


    /**
     * Builds the OFF-before-ON dispatch order from every candidate
     * target. Preserves the caller's relative ordering within each phase
     * — passing targets in Best-First admission order (the order
     * BestFirstSearch::isLoadSelectedToBeOn() was evaluated in) keeps
     * that same priority order within the ON phase.
     *
     * A target whose desiredOn already equals currentlyConfirmedOn is a
     * no-op and is dropped from the result — there is nothing to command.
     *
     * Always compiled and hardware-free: no ESP-NOW/RelayController
     * dependency, so directly host-testable.
     */
    static std::vector<RelayTarget> buildDispatchOrder(const std::vector<RelayTarget>& targets);


    RelayCommandDispatcher();


    /**
     * Allocates a new commandId, records target as pending at
     * nowMilliseconds, and returns the allocated commandId. commandIds
     * are unique for the lifetime of this object (monotonically
     * increasing), so a late acknowledgement for a superseded command can
     * never be confused with the current one.
     */
    std::uint32_t beginCommand(const RelayTarget& target, std::uint32_t nowMilliseconds);


    /** Returns true while commandId has been begun but not yet completed/expired. */
    bool isPending(std::uint32_t commandId) const;


    /** Returns the pending record for commandId, or nullptr when it is not pending. */
    const PendingCommand* findPendingCommand(std::uint32_t commandId) const;


    /**
     * Removes commandId from the pending set once its outcome (success or
     * failure) has been handled by the caller — this class does not
     * itself decide what a confirmed/failed command means for the owning
     * Load; it only stops tracking it.
     *
     * Returns false when commandId was not pending.
     */
    bool completeCommand(std::uint32_t commandId);


    /**
     * Returns every pending command whose dispatchedAtMilliseconds is
     * more than timeoutMilliseconds before nowMilliseconds — candidates
     * for the caller to treat as a relay-confirmation failure. Does not
     * remove them; the caller calls completeCommand() after handling
     * each one, exactly as it would for a real acknowledgement.
     */
    std::vector<PendingCommand> findExpiredCommands(std::uint32_t nowMilliseconds, std::uint32_t timeoutMilliseconds) const;


    /** Returns how many commands are currently pending. */
    std::size_t getNumberOfPendingCommands() const;


    /** Clears every pending command without resetting the commandId counter. */
    void clearPendingCommands();


private:

    std::vector<PendingCommand> pendingCommands_;
    std::uint32_t nextCommandId_;
};


} // namespace kilowatts

#endif // KILOWATTS_RELAY_COMMAND_DISPATCHER_H
