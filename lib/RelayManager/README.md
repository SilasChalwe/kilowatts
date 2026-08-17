# RelayManager

Physical relay actuation and the MQTT/ESP-NOW command-to-relay
dispatch that drives it — grouped by domain, each class still
strictly single-responsibility per its own section below.

## Folder layout: Central / shared

`RelayController` is the shared class both firmwares build (each Node
actuates its own relays). `RelayCommandDispatcher` is Central-only
(it turns a Best-First Search result into dispatch order) and lives
under `Central/`. `platformio.ini`'s shared `[env]` `build_flags` adds
`-Ilib/RelayManager/Central` for the same reason described in
`lib/NodeManager`'s README.

---

## RelayController


Drives the physical relay GPIO pins THIS Node owns, and reports back the
commanded/read-back state honestly. This is the module that finally turns
a Best-First Search decision into a real electrical ON/OFF transition.

## Responsibility

```cpp
RelayController relays;

relays.addRelay(RelayController::RelayConfiguration{
    /* relayPin       = */ 16U,
    /* activeHigh     = */ false,  // this relay board is active-LOW
    /* initialStateOn = */ false   // safe default: OFF at boot
});

relays.setRelayState(16U, true);   // switch that Load ON

bool commanded = false;
relays.getCommandedState(16U, commanded);   // what was last commanded

bool confirmed = false;
relays.readBackState(16U, confirmed);       // real GPIO read-back
```

- `relayPin` is the same relay pin identity used everywhere else in the
  system (`Load::getRelayPin()`, `BestFirstSearch::BranchId::relayPin`).
- `activeHigh` is never assumed — a physical relay board's ON polarity is
  a property of that board, so the caller (Smart/Central Node
  configuration) always supplies it explicitly, the same convention
  `INA219Monitor` already follows for shunt resistance.
- `addRelay()` configures the GPIO as an output and drives it to
  `initialStateOn` in the same call, so a registered relay is never left
  floating in an undefined state. The document's physical 10 kOhm
  pull-down resistor on each relay control line is the hardware-side
  guard for the short window before this call runs during boot;
  `pull_down_en` is enabled here as the matching software-side default.
- `getCommandedState()` is bookkeeping only (what this object last told
  the GPIO to do); `readBackState()` re-reads the real GPIO output level
  and translates it back through `activeHigh`. A caller comparing the two
  after a command is how a stuck/failed relay is detected — this module
  does not decide what to do about a mismatch, only reports it.

## Host build vs. ESP32 target

Relay registration and commanded-state tracking are plain, hardware-free
C++ and always compiled, so they are exercised by
`test/RelayController/test_relay_controller.cpp` with no ESP32 involved.
Real `gpio_config()`/`gpio_set_level()`/`gpio_get_level()` calls are
compiled only under `ESP_PLATFORM` (see `RelayController.cpp`), matching
the split `INA219Monitor` and `CurrentTimeProvider` already use. A host
build's `setRelayState()`/`readBackState()`/`isHardwareApplied()` always
report failure/false rather than fabricating a GPIO level.

## Boundary

`RelayController` does not decide *which* relay pins should be ON — that
is `BestFirstSearch`'s result together with the OFF-before-ON dispatch
order (see `RelayCommandDispatcher`). It does not perform ESP-NOW
communication (see `EspNowCommunication` and the `RelayCommandPacket` /
`RelayCommandAcknowledgementPacket` wire packets in
`NodeReportPackets.h` in `lib/NodeManager`), does not know which Load a relay pin represents
in the wider system, and does not run on the Central Node's planning
logic — it only actuates GPIOs local to whichever Node it runs on.

---

## RelayCommandDispatcher


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

