# NodeIdentityStore

A Smart Node's own local, persisted commissioning identity — the
Smart-Node-local counterpart to Central's authoritative
`NodeCommissioningRegistry`. Each Smart Node stores only its own identity,
never the whole installation.

## Responsibility

```cpp
NodeIdentityStore identity;
identity.loadPersisted();   // resume whatever was saved last (ESP32 target)

identity.getLifecycleState();   // UNCOMMISSIONED until commissioned
identity.getFriendlyName();     // "" until commissioned

// A CommissionCommandPacket arrived over ESP-NOW:
const bool accepted = identity.applyCommission(command.friendlyName);
identity.persist();
// reply with CommissionAckPacket{command.commandId, accepted, identity.getLifecycleState()}

// A DecommissionCommandPacket arrived:
identity.applyDecommission();
identity.persist();
```

## Why this is simpler than NodeCommissioningRegistry

Central's `NodeCommissioningRegistry` observes a Node from the outside, so
it needs `DISCOVERED`/`CONFIGURING` and a pending/confirm split (the round
trip might fail, be delayed, or race). A Node observing *itself* needs
neither: `applyCommission()` resolves synchronously and completely in one
call — a Node is its own authority for whether it accepted a friendly name,
so there is nothing further to confirm locally. This class's
`lifecycleState` therefore only ever holds `UNCOMMISSIONED`, `COMMISSIONED`
or `OPERATIONAL` — never `FACTORY`/`DISCOVERED`/`CONFIGURING` (those only
describe what Central currently knows *about* a Node) and never
`DECOMMISSIONED` (`applyDecommission()` resets straight back to
`UNCOMMISSIONED`, since a Node has no reason to remember it was once
decommissioned — only Central needs that historical distinction, to avoid
recreating a fake Node from a stale command).

`applyCommission()`'s acceptance rule mirrors
`NodeCommissioningRegistry::requestCommissioning()` exactly (non-empty name
that fits the buffer; current state `UNCOMMISSIONED` for a first
commissioning or `COMMISSIONED`/`OPERATIONAL` for a rename) since the two
are the Node-local and Central-authoritative halves of the same operation.

## Persistence

NVS namespace `kw_identity`, schema-versioned. A persisted `UNCOMMISSIONED`
record (no friendly name) is a legitimate, honestly-restored value — not
treated as corrupt — since a decommissioned or never-commissioned Node
persisting "no identity" is exactly the state it should restore to after a
reboot. A persisted `COMMISSIONED`/`OPERATIONAL` record with an
empty/invalid friendly name, or any other lifecycle value, is corrupt and
discarded.

## Host build vs. ESP32 target

`getLifecycleState()`/`getFriendlyName()`/`applyCommission()`/
`applyDecommission()` are plain, hardware-free C++, always compiled, and
directly host-testable (see `test/NodeIdentityStore/`). NVS persistence
(`loadPersisted()`/`persist()`) is compiled only under `ESP_PLATFORM`; a
host build's `persist()`/`loadPersisted()` always report failure rather
than fabricating durable storage.

## Boundaries

This class does not perform ESP-NOW communication itself
(`CommissioningPackets`/`EspNowCommunication`), does not decide *when* a
commissioning/decommission command should be sent — it only applies one a
caller already received — and does not know about GPIO/I2C/Branches/Loads
(later phases).
