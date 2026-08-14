# RelayCommandDispatcher

Sequences relay commands OFF-before-ON (Section 4.6.4.3, Algorithm 4.6)
and tracks each dispatched command until it is confirmed or expires.

## Responsibility

```cpp
std::vector<RelayCommandDispatcher::RelayTarget> targets = /* one per Load whose
    target state (BestFirstSearch::isLoadSelectedToBeOn() + Fixed allocation)
    may differ from Load::getConfirmedRelayState() */;

const auto dispatchOrder = RelayCommandDispatcher::buildDispatchOrder(targets);

RelayCommandDispatcher dispatcher;
for (const auto& target : dispatchOrder) {
    const std::uint32_t commandId = dispatcher.beginCommand(target, nowMilliseconds);
    // send a RelayCommandPacket carrying commandId to target.nodeMacAddress
    // (over ESP-NOW, or directly through RelayController for Central's own
    // relays) — RelayCommandDispatcher does not send it itself.
}

// On a RelayCommandAcknowledgementPacket / local confirmation:
dispatcher.completeCommand(acknowledgedCommandId);

// Periodically:
for (const auto& stale : dispatcher.findExpiredCommands(nowMilliseconds, timeoutMilliseconds)) {
    // treat as a relay-confirmation failure, then:
    dispatcher.completeCommand(stale.commandId);
}
```

`buildDispatchOrder()` is a pure function: it drops every target already
at its desired confirmed state (nothing to command), then returns every
**OFF** transition first (so capacity is released before anything new is
energised), followed by every **ON** transition in the caller's original
order — passing targets in Best-First admission order therefore keeps
that same priority order within the ON phase, matching "Loads selected
for ON operation are then enabled in Best-First order."

Command tracking (`beginCommand()`/`completeCommand()`/
`findExpiredCommands()`) allocates a unique, monotonically increasing
`commandId` per dispatched command so a late or duplicate acknowledgement
can never be confused with the command currently in flight for that relay.

## Boundary

This module never sends an ESP-NOW message (`EspNowCommunication`) and
never actuates a GPIO (`RelayController`) — it only decides sequencing
and tracks in-flight commands by `commandId`. It does not decide *what*
the target schedule is (that is `BestFirstSearch` plus Fixed-Load
allocation) and does not interpret *why* a command failed — the caller
decides what a timeout or a failed acknowledgement means for the owning
`Load` (`Load::setHealth(LoadHealth::FAULTED)`, a maintenance/diagnostic
event, etc.).

Entirely plain, hardware-free/network-free C++ — no `ESP_PLATFORM` split
is needed, so it is directly host-testable (see
`test/RelayCommandDispatcher/`).
