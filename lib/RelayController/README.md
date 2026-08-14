# RelayController

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
`lib/NodeReportPackets`), does not know which Load a relay pin represents
in the wider system, and does not run on the Central Node's planning
logic — it only actuates GPIOs local to whichever Node it runs on.
