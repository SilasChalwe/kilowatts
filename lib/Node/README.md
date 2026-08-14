# Node

Represents one ESP32 in the system and owns the Loads physically wired to
it.

## Responsibility

A `Node` is identified globally by its ESP32 MAC address, and owns a
`std::vector<Load>` of the Loads connected to it:

```cpp
Node sittingRoom(localMacAddress);

sittingRoom.addLoad(Load({localMacAddress, 16U}, "Light", {12.0F, 12.0F}, 1U, LoadMode::Fixed::ON));
sittingRoom.addLoad(Load({localMacAddress, 17U}, "Router", {10.0F, 10.0F}, 2U, LoadMode::Fixed::ON));

Load* fan = sittingRoom.getLoadByRelayPin(18U);
```

`addLoad()` enforces the two rules that keep a Node's Loads consistent:

- the Load's `Id.macAddress` must match this Node's own MAC address — a
  Node cannot silently adopt a Load that belongs to a different Node, and
- no two Loads on the same Node may share a relay pin.

`getLoadByRelayPin()` is the lookup the rest of the system relies on to
turn a local relay pin (for example one read back from an INA219 sensor
association, see `INA219Monitor`) into the actual `Load` object to update.

`removeLoadByRelayPin()` removes the Load on a given relay pin, if this
Node has one, and returns whether it actually removed something. This is
how `CentralNodeRegistry::applyNodeReport()` prunes a Load a Node no
longer reports — a Node's real Load count can legitimately shrink (most
notably to zero right after commissioning, since a freshly
flashed/uncommissioned Node reports none), and Central's view of that
Node must not keep a stale Load around forever just because it was once
reported. Once removed, the relay pin is free to be reused by a
completely different Load later — `removeLoadByRelayPin()` does not
reserve or remember the pin in any way.

## Boundaries

`Node` only owns and looks up its own Loads. It does not know about other
Nodes, does not perform ESP-NOW communication or discovery
(`EspNowCommunication`), does not track network topology
(`CentralNodeRegistry`), and does not decide which Loads should be ON
(`BestFirstSearch`).
