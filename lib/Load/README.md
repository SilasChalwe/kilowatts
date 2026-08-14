# Load

Represents one electrical load connected to one ESP32 Node — the
fundamental domain object every other Load/Node-aware module in this
project builds on.

## Identity

A Load's identity is its owning Node's MAC address plus its relay pin,
never an arbitrary integer ID:

```cpp
struct Load::Id {
    MacAddress macAddress;   // the owning Node's real Wi-Fi MAC address
    std::uint8_t relayPin;   // which relay pin on that Node
};
```

This lets Loads from many different Nodes be combined, filtered, searched
and reported back through the network without losing track of which
physical Node — and which relay pin on that Node — each one belongs to.

## What one Load stores

```cpp
Load fan(
    Load::Id{sittingRoomMac, 18U},
    "Fan",
    LoadPower{18.0F, 25.0F},   // running watts, startup watts
    3U,                        // priority
    LoadMode::Auto::ON
);
```

- **`LoadMode`** — one combined value for both how the Load is controlled
  and its current state: `Fixed::ON`, `Fixed::OFF`, `Auto::ON`,
  `Auto::OFF`. There is no separate on/off flag alongside a separate
  fixed/auto flag — `setMode()`/`getMode()` always move all four
  combinations together, and `isFixed()`/`isAuto()`/`isOn()`/`isOff()`
  are derived from that one value.
- **`LoadPower`** — the *configured* running/startup wattage used for
  planning (`startupWatts` must never be lower than `runningWatts`;
  `setPower()` rejects a violation).
- **`LoadMeasurements`** — the *live* voltage/current/power most recently
  measured for this Load (see `INA219Monitor`). `setMeasurements()`
  rejects a non-finite or negative value rather than silently storing it.
  `LoadMeasurements` and `LoadPower` are deliberately separate: a live
  reading never overwrites the configured running/startup wattage.
- **`AutoSchedule`** — an optional preferred running time (`hour`,
  `minute`) for an Auto Load only; `setSchedule()` rejects the call when
  the Load is Fixed, and `setMode()` clears any schedule the moment a
  Load becomes Fixed.
- **Confirmed relay state** (`setConfirmedRelayState()`/
  `getConfirmedRelayState()`) — the relay state last read back from real
  hardware (`RelayController::readBackState()`), deliberately separate
  from `getMode()`/`isOn()` (the configured/commanded intent). A caller
  must never treat one as the other, especially while a relay command is
  in flight or has failed.
- **`LoadHealth`** (`setHealth()`/`getHealth()`) — `AVAILABLE`,
  `FAULTED` or `UNAVAILABLE`, as last reported by the owning Node. Also
  independent of `LoadMode`: health describes whether the physical relay
  can currently be trusted, not how it is configured.

## Boundaries

`Load` only stores and validates one load's own state. It does not decide
which Loads should be ON (`BestFirstSearch`), does not classify Loads into
Fixed/Auto groups (`LoadFilter`), does not know about other Loads on the
same Node (`Node`), and does not read real sensor hardware
(`INA219Monitor`).
