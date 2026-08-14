# NodeCommissioningRegistry

Central's authoritative record of every Node's identity and commissioning
lifecycle — separate from `CentralNodeRegistry`'s planning-time domain data.

## Responsibility

```cpp
NodeCommissioningRegistry registry;
registry.loadPersisted();   // resume whatever was saved last (ESP32 target)

// A real IdentityReportPacket arrived over ESP-NOW:
const bool isNewNode = registry.recordDiscovered(mac, NodeRole::SMART, "0.2.0-foundation", "esp32:2core", nowMs);
// isNewNode -> emit a NODE_DISCOVERED event (see src/central/main.cpp)

// Central's own local identity, no round trip needed:
registry.registerSelf(localMac, NodeRole::CENTRAL, "Central", KILOWATTS_FIRMWARE_VERSION, chipModelText, nowMs);

// An MQTT COMMISSION_NODE or RENAME_NODE command was accepted (see
// MqttManager::ConfigCommandRequest / src/central/main.cpp's
// handleConfigCommand()):
if (registry.requestCommissioning(mac, "Sitting Room")) {
    // send CommissionCommandPacket{commandId, "Sitting Room"} over ESP-NOW
}

// The Node's own CommissionAckPacket arrived:
registry.applyCommissionResult(mac, ack.success != 0U, ack.resultingState);

// An MQTT DECOMMISSION_NODE command was accepted:
if (registry.decommission(mac)) {
    // send DecommissionCommandPacket over ESP-NOW as best-effort notice
}

registry.persist();
```

## Two concerns, deliberately not merged

`CentralNodeRegistry` already owns the real `Node`/`Load` domain objects and
network topology (`nextHopToCentralMacAddress`, `hopCountToCentral`,
`lastSeenMilliseconds`) converted from `NodeReportPacket` traffic.
`NodeCommissioningRegistry` owns identity/lifecycle/commissioning-sync data
only — role, `NodeLifecycleState`, friendly name, firmware version, chip
model, `SyncState`. Neither duplicates the other's fields; a caller that
needs both (for example to publish `state/nodes`, see `NodeRegistryJson`)
joins the two by MAC address.

## One round trip, two commands

`requestCommissioning()` is used for both a Node's *first* commissioning
(current state `UNCOMMISSIONED`/`DISCOVERED` → `CONFIGURING`) and a *rename*
of an already-commissioned Node (state `COMMISSIONED`/`OPERATIONAL` stays
unchanged) — both send the exact same `CommissionCommandPacket` over the
wire, since to the Node itself "apply this friendly name" is the same
operation either way. `applyCommissionResult()` always trusts the Node's own
reported `resultingState` (the Node itself is authoritative for whether it
actually applied the change — the same principle `CommissionAckPacket`'s own
doc comment states) rather than Central assuming success from having sent
the command; on failure, `pendingFriendlyName` is discarded and
`friendlyName` is left exactly as it was, so a failed rename can never
partially apply.

## Rediscovering a decommissioned Node

`NodeLifecycle` only allows `DECOMMISSIONED` to exit toward
`UNCOMMISSIONED` — `recordDiscovered()` is the one place that transition
happens, when a previously decommissioned MAC address is heard from again
(the device is physically present and broadcasting, so it becomes eligible
for re-commissioning rather than requiring a separate "undo decommission"
command).

## Persistence

Only records that reached `COMMISSIONED`, `OPERATIONAL` or `DECOMMISSIONED`
are ever persisted — a transient `UNCOMMISSIONED`/`DISCOVERED`/`CONFIGURING`
record is always rebuilt fresh from real ESP-NOW discovery after a reboot,
never restored from a stale snapshot. Storage is one schema-versioned blob
in NVS namespace `kw_commission` (the same "one blob, not one NVS key per
record" pattern `LoadConfigurationStore` already uses); an unsupported
schema version or a corrupt/malformed blob is discarded rather than
accepted, and this class never reconstructs demo records as a fallback.

## Host build vs. ESP32 target

Every lookup/lifecycle method (`recordDiscovered()`, `registerSelf()`,
`requestCommissioning()`, `applyCommissionResult()`, `decommission()`,
lookups) is plain, hardware-free C++, always compiled, and directly
host-testable (see `test/NodeCommissioningRegistry/`). NVS persistence
(`loadPersisted()`/`persist()`) is compiled only under `ESP_PLATFORM`; a
host build's `persist()`/`loadPersisted()` always report failure rather than
fabricating durable storage.

## Boundaries

This class does not perform ESP-NOW communication itself (`EspNowCommunication`,
`CommissioningPackets`), does not decide *when* a commissioning/decommission
command should be sent (`src/central/main.cpp`), does not own GPIO, battery
I2C, Branches or Loads (SmartNodeConfigurationStore clears a Node's local
load configuration during confirmed decommission), and does not calculate route/Hop Count/RSSI
or hold real `Node`/`Load` domain objects (`CentralNodeRegistry`).
