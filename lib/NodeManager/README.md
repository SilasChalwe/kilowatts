# NodeManager

Node identity, commissioning, lifecycle, topology and the wire-packet
definitions used to report/configure/commission nodes over ESP-NOW and
MQTT — grouped into one library because they all describe or manage a
Node, even though each class below keeps its own single responsibility
and does not reach into another's job.

## Folder layout: Central / Smart / shared

Classes used by only one firmware role live in their own role subfolder;
classes both firmwares build (the Node domain object, the lifecycle state
machine shared by both, and the ESP-NOW wire packets both ends
serialize/deserialize) stay directly in this folder:

- `Central/` — `NodeCommissioningRegistry`, `NodeRegistryJson`,
  `CentralNodeRegistry`, `TopologyTree`, `CentralConfigurationStore`
  (all Central-only: fleet registry, MQTT JSON, planning-time view,
  topology, Central's own NVS config).
- `Smart/` — `NodeIdentityStore` (Smart-only: a Smart Node's own local
  identity/config).
- Here (shared) — `Node`, `NodeLifecycle`, `NodeReportPackets`,
  `CommissioningPackets`, `HardwareConfigurationPackets`, and
  `NodeLoadHardwareStore` (each firmware role constructs and persists its
  own separate instance — Central's own locally-wired Loads and a Smart
  Node's own locally-wired Loads are never the same store or the same NVS
  data, just the same reusable class, since the two roles are always
  separate physical devices). `ChipInfo` moved out to `lib/FirmwareManager`
  (general firmware infrastructure, not Node-specific) and
  `DevelopmentSession`/`DevelopmentPackets` moved out to
  `lib/DevelopmentManager` (simulated/development-only, kept strictly
  apart from production code) — see those libraries' own READMEs.

PlatformIO's Library Dependency Finder only exposes this folder's own
root as an include path, not `Central/`/`Smart/` — `platformio.ini`'s
shared `[env]` `build_flags` adds `-Ilib/NodeManager/Central` and
`-Ilib/NodeManager/Smart` explicitly so a bare `#include "X.h"` from
outside this folder (e.g. another library) can still find a nested
header. `run_cpp_test.sh` resolves headers by searching all of `lib/`
recursively, so it needs no such extra configuration.

---

## Node


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

---

## NodeCommissioningRegistry


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
registry.registerSelf(localMac, NodeRole::CENTRAL, "Central", KILOWATTS_FIRMWARE_VERSION, chipModelText,
                       CentralNodeConfig::VERIFIED_RELAY_GPIO_PINS.data(),
                       CentralNodeConfig::VERIFIED_RELAY_GPIO_PINS.size(), nowMs);

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
I2C, Branches or Loads (NodeLoadHardwareStore clears a Node's local
load configuration during confirmed decommission), and does not calculate route/Hop Count/RSSI
or hold real `Node`/`Load` domain objects (`CentralNodeRegistry`).

---

## NodeIdentityStore


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

---

## NodeLifecycle


The device commissioning lifecycle shared by both Kilowatts firmware roles —
header + a small `.cpp` of pure transition-validation logic, no wire format
and no persistence of its own.

## Responsibility

```cpp
NodeLifecycleState state = NodeLifecycleState::UNCOMMISSIONED;

if (isValidNodeLifecycleTransition(state, NodeLifecycleState::DISCOVERED)) {
    state = NodeLifecycleState::DISCOVERED;
}

toText(state);   // "discovered" - for logs/JSON
```

```
FACTORY -> UNCOMMISSIONED
UNCOMMISSIONED -> DISCOVERED | CONFIGURING | DECOMMISSIONED
DISCOVERED -> CONFIGURING | DECOMMISSIONED
CONFIGURING -> COMMISSIONED | UNCOMMISSIONED | DECOMMISSIONED
COMMISSIONED -> OPERATIONAL | DECOMMISSIONED
OPERATIONAL -> DECOMMISSIONED
DECOMMISSIONED -> UNCOMMISSIONED
```

A newly flashed device with no valid persisted commissioning record boots
`UNCOMMISSIONED` (never `FACTORY` beyond the instant before that first check
resolves it). `CONFIGURING` is only ever entered from `UNCOMMISSIONED`/
`DISCOVERED` for a Node's *first* commissioning — renaming an already-
`COMMISSIONED`/`OPERATIONAL` Node never revisits `CONFIGURING` (see
`NodeCommissioningRegistry::rename()`), so a failed/timed-out commissioning
attempt can unambiguously roll back to `UNCOMMISSIONED` without ever
un-commissioning a Node that was already working. `OPERATIONAL` is reserved
for a later phase once a Node can actually own Branches/Loads; nothing in
this phase produces it.

`isValidNodeLifecycleTransition()` never treats a state as validly
transitioning to itself — a caller that only needs to refresh bookkeeping
(for example "last seen" timestamps) for an already-known state should not
call it at all.

`MacAddress` here is the same six-byte layout `Load::MacAddress` and
`EspNowCommunication::MacAddress` already use, declared independently
(rather than included from either) so the commissioning libraries that
build on this header stay free of any ESP-IDF or domain-object dependency
and remain host-testable.

## Boundaries

`NodeLifecycle` only defines the state values and which transitions between
them are structurally legal. It does not decide *when* a transition should
happen (`NodeCommissioningRegistry` on Central, `NodeIdentityStore` on a
Smart Node), does not perform ESP-NOW communication
(`CommissioningPackets`/`EspNowCommunication`), and does not persist
anything.

---

## NodeRegistryJson


Formats `NodeCommissioningRegistry`'s known Nodes into the two JSON payloads
the commissioning MQTT contract needs: `kilowatts/v1/state/nodes` and
`kilowatts/v1/config/nodes` — the same role `TopologyTree` already fills for
`state/tree`/`state/loads`.

## Responsibility

```cpp
const std::string stateNodesJson = NodeRegistryJson::buildStateNodesJson(
    commissioningRegistry, centralNodeRegistry, CentralNodeConfig::MQTT_SCHEMA_VERSION,
    nowMilliseconds, CentralNodeConfig::NODE_REPORT_TIMEOUT_MILLISECONDS);

const std::string configNodesJson = NodeRegistryJson::buildConfigNodesJson(
    commissioningRegistry, CentralNodeConfig::MQTT_SCHEMA_VERSION);
```

`state/nodes` carries one object per `NodeCommissioningRegistry` record
(identity: MAC, role, name, lifecycle/sync state, firmware version, chip
model), joined by MAC address with `CentralNodeRegistry`'s route/last-seen
data **where a matching `PlanningNode` exists yet** — a Node discovered by
`IdentityReportPacket` but that has not sent a `NodeReportPacket` yet has
none, in which case `"online"`/`"hopCountToCentral"`/`"nextHopMac"`/
`"lastSeenMilliseconds"` are JSON `null` rather than fabricated, matching
the "keep unavailable values explicit" rule every other diagnostic payload
in this project already follows.

`config/nodes` carries only commissioned identity — MAC, role, name,
lifecycle/sync state — never route/online/telemetry, matching the
state-vs-configuration boundary every other `config/*` topic follows
(compare `TopologyTree`'s `state/tree` vs. what a later phase's
`config/branches`/`config/loads` will carry).

**Communication topology is not electrical Branch ownership here either**
— see `TopologyTree`'s own README for why Hop Count/Next Hop are never used
as Branch/Node configuration identity. This module only reports the route
`CentralNodeRegistry` already has for informational/diagnostic purposes.

## Host build

Like `TopologyTree`, this module depends on `CentralNodeRegistry.h` (through
`NodeReportPackets.h` → `EspNowCommunication.h`), which requires the real
ESP-IDF/FreeRTOS headers, so it is ESP32-target-only with no host-native
test. Its actual formatting logic is plain string concatenation with no
further hardware dependency of its own — the same tradeoff `TopologyTree`
already documents, not a new pattern.

## Boundary

Does not discover Nodes, does not perform ESP-NOW communication, does not
decide commissioning outcomes (`NodeCommissioningRegistry`), and does not
publish anything (`MqttManager` does that) — it only reads what
`NodeCommissioningRegistry`/`CentralNodeRegistry` already know and formats
it.

---

## NodeReportPackets


The shared wire-format structures sent between Kilowatts ESP32 firmware
over ESP-NOW — header-only, no `.cpp`.

## Responsibility

These structures are the byte layout carried inside an
`EspNowCommunication::Message` payload. They are a **transport**
representation only — a `NodeReportPacket` is how one Node's report
travels over the radio, not the `Node`/`Load` domain object used for
planning (see `Node` and `Load` for that; `CentralNodeRegistry` is what
converts one into the other).

This module was previously named `PowerManagementMessages`, which was a
misleading name: it does not manage power. Every structure here is an
actual ESP-NOW wire-format payload, not a general DTO and not a
power-management domain object — hence `Packet` in every type name.

```cpp
struct LoadReportPacket {
    char name[16];
    std::uint8_t relayPin;
    std::uint8_t mode;                     // raw kilowatts::LoadMode::Value byte (configured)
    std::uint16_t priority;
    float startupWatts;
    float branchMaximumCurrentAmps;        // this relay pin's Branch configuration, I_branch,max
    float nominalVoltageVolts;             // installer/nameplate rating, not live sensor data
    float nominalCurrentAmps;              // installer/nameplate rating, not live sensor data
    std::uint8_t confirmedRelayState;      // last GPIO read-back (0=OFF, 1=ON) — physical truth
    std::uint8_t confirmedRelayStateValid; // Load::isConfirmedRelayStateValid(): 1=trustworthy, 0=unknown
    std::uint8_t scheduleEnabled;
    std::uint8_t scheduleHour;
    std::uint8_t scheduleMinute;
    std::uint8_t availability;             // raw LoadAvailability byte
};

struct NodeReportPacket {
    char nodeName[20];
    EspNowCommunication::MacAddress nodeMacAddress;
    EspNowCommunication::MacAddress upstreamNodeMacAddress;   // this Node's Next Hop to Central
    std::uint16_t hopCountToCentral;
    std::uint8_t numberOfLoads;
    std::uint16_t reportSequenceId;
    std::uint8_t pageIndex;                // reserved; currently must be 0
    std::uint8_t totalPages;               // reserved; currently must be 1
    std::array<LoadReportPacket, MAX_LOADS_PER_NODE_PACKET> loads;
};

struct NodeReportAcknowledgementPacket {
    EspNowCommunication::MacAddress nodeMacAddress;
    std::uint32_t receivedMessageId;
};

struct RelayCommandPacket {
    std::uint8_t relayPin;
    std::uint8_t desiredState;             // raw RelayCommandState byte
    std::uint32_t commandId;
};

struct RelayCommandAcknowledgementPacket {
    std::uint8_t relayPin;
    std::uint32_t commandId;
    std::uint8_t requestedState;           // raw RelayCommandState byte
    std::uint8_t confirmedState;           // raw RelayCommandState byte, valid only when success=1
    std::uint8_t success;
    std::uint8_t failureReason;            // raw RelayCommandFailureReason byte
};
```

`MAX_LOADS_PER_NODE_PACKET = 3` is the current **per-Smart-Node configured
load limit** (the complete report must fit inside one ESP-NOW message; ESP-NOW v1.0, which the
Central Node's classic ESP32 is limited to, caps one message at 250
bytes). `pageIndex`/`totalPages` are reserved fields, but the current Central
and Smart firmware support only one complete report (`pageIndex = 0`,
`totalPages = 1`) and Central rejects a partial/multi-page report. Increasing
the per-node limit requires a future release that implements complete
multi-page sending and reassembly on both sides.

The destination Node for a `RelayCommandPacket` /
`RelayCommandAcknowledgementPacket` is already carried by the surrounding
`EspNowCommunication::Message` header
(`destinationMacAddress`/`originMacAddress`) — these packets only ever
identify *which relay pin on that Node*, never a Node MAC address again.
`BestFirstSearch::BranchId` (and a `LoadReportPacket`'s Branch identity)
is always reconstructed as `{NodeReportPacket::nodeMacAddress,
LoadReportPacket::relayPin}` for the same reason: a Load's owning Node is
already known from the enclosing report, so it is never repeated per Load.

Central and Smart firmware previously each declared their own copy of
these structures by hand. Because both sides must agree on the exact same
byte layout to interpret a received payload correctly, the two copies were
a system contract that could silently drift apart if only one file was
edited — they are declared once here instead and included by both
`src/central/main.cpp` and `src/smart/main.cpp`.

Several `static_assert`s enforce the contract at compile time: every
structure here must be trivially copyable (safe to send/receive as raw
bytes over ESP-NOW), and `NodeReportPacket`/`RelayCommandPacket`/
`RelayCommandAcknowledgementPacket` must each fit inside
`EspNowCommunication::MAX_PAYLOAD_SIZE`.

## Boundaries

Smart Nodes have no per-load INA219 in the final design: their nominal
voltage/current fields are installation ratings, and Central derives their
planned power as V × A. It must be presented as an estimate. Central's
separate battery-bus INA219 is the sole live current sensor.

This header defines byte layout only. It does not perform the actual
send/receive (`EspNowCommunication`), does not decide message routing or
topology, does not read INA219 hardware or actuate relays (`INA219Monitor`,
`RelayController`), and does not decide *when* a report is sent or *what*
relay state to command — it only describes what those decisions travel as
over the radio.

---

## CentralNodeRegistry


Central's planning-time view of every Node it knows about — the layer that
turns raw `NodeReportPacket`/`LoadReportPacket` transport bytes into real
`kilowatts::Node`/`kilowatts::Load` domain objects.

## Responsibility

The Central Node receives `NodeReportPacket` reports over ESP-NOW. A
`NodeReportPacket` is a transport representation: raw bytes describing what one
Node currently looks like, not the domain object used for planning.
`CentralNodeRegistry` converts each received report into real `Node`/
`Load` objects and keeps one up-to-date copy of every Node Central
currently knows about — including Central's own local Node:

```cpp
CentralNodeRegistry registry;

registry.addLocalCentralNode("Central", centralNode);   // Central's own directly-wired Loads
registry.applyNodeReport(receivedNodeReportPacket);       // every remote Node, as reports arrive

registry.getNumberOfNodes();
const CentralNodeRegistry::PlanningNode* node = registry.getNode(i);
registry.findNodeByMacAddress(someMacAddress);
```

Each `PlanningNode` bundles one Node's real domain object together with
the network topology Central currently has for it (`nodeName`,
`nextHopToCentralMacAddress`, `hopCountToCentral`, `isCentralNode`), so a
caller does not need a second lookup to relate a Node's Loads to how
Central currently reaches it.

`applyNodeReport()`'s first report for a given Node MAC address creates a
new `PlanningNode`; every later report for the same MAC address updates
that `PlanningNode`'s topology fields and updates the matching Load (found
by relay pin) rather than creating a duplicate, so the same physical Load
keeps its identity across repeated reports.

A Load this Node no longer reports is pruned (`Node::removeLoadByRelayPin()`)
rather than left behind — a Node's real Load count can legitimately shrink,
most notably to zero right after commissioning (a freshly flashed/
uncommissioned Node reports none), and this registry must stay an honest,
up-to-date mirror of what each Node currently reports, not an
ever-accumulating history of every Load it has ever mentioned. The current
wire contract supports only a complete, single-page report (`pageIndex == 0
&& totalPages == 1`); `applyNodeReport()` rejects any other values before
they can change or prune the planning topology. A future increase beyond
three configured Loads per Smart Node must implement multi-page sending and
reassembly on both sides first.

A Load's identity is its owning Node's MAC address plus its relay pin
(`Load::Id`). `CentralNodeRegistry` always takes that owning MAC address
from the report's own `NodeReportPacket::nodeMacAddress` field, never from the
immediate ESP-NOW sender of the packet — a Node that forwards a report on
behalf of one of its own descendants must not have that descendant's Loads
reassigned to itself.

## Boundaries

`CentralNodeRegistry` holds and updates this data only. It does not
perform ESP-NOW discovery, does not calculate RSSI or Hop Count itself
(it only stores what a `NodeReportPacket` reports), does not calculate battery
State of Charge or Available Power, does not run Best-First Search, does
not classify Loads (`LoadFilter`), does not perform MQTT communication,
and does not control relays.

---

## TopologyTree


Formats `CentralNodeRegistry`'s known Nodes/Loads/Branches into the two
JSON payloads the mobile application needs: a nested tree
(`kilowatts/v1/state/tree`) and a flat per-Load array
(`kilowatts/v1/state/loads`).

## Responsibility

```cpp
const std::string treeJson = TopologyTree::buildTreeJson(
    registry, CentralNodeConfig::MQTT_SCHEMA_VERSION,
    nowMilliseconds, /* onlineTimeoutMilliseconds */ 30000U);

const std::string loadsJson = TopologyTree::buildLoadsJson(registry, CentralNodeConfig::MQTT_SCHEMA_VERSION);
```

The tree's shape:

```
Central
└── Smart Node ("online": true/false, from PlanningNode::lastSeenMilliseconds)
    ├── Branch (nodeMac + relayPin, Branch's I_branch,max) → Load
    ├── Branch → Load
    └── children: [ ... further Smart Nodes, nested by their real Next-Hop ]
```

**Communication topology is kept distinct from electrical Branches.** A
Node's *position* in the tree comes from
`PlanningNode::nextHopToCentralMacAddress` (who it actually forwards
through — ESP-NOW routing); the *Branches* listed under a Node come from
its own relay pins (`BestFirstSearch::BranchId` = that Node's MAC +
relay pin) and each Branch's configured `maximumCurrentAmps`
(`CentralNodeRegistry::BranchConfiguration`). Hop Count and Next Hop are
never used as a Branch identity — see `BestFirstSearch`'s own README for
why.

Each Branch's `load` object, and every object in `buildLoadsJson()`'s flat
array, carries the full field set the frontend needs without recomputing
anything: relay pin, name, owning Node MAC, configured `mode` versus
**target** state (`Load::getTargetRelayState()` — the actual per-cycle
decision Central's Best-First Search wrote for this Load this cycle, which
for an Auto Load can legitimately disagree with `mode`, e.g. a configured
`AUTO_ON` Load that this cycle's search rejected for limit/battery reasons
reports `targetOn: false`) versus **confirmed** state
(`Load::getConfirmedRelayState()` plus `Load::isConfirmedRelayStateValid()`
— the last hardware-confirmed physical truth, and whether that confirmation
is still trustworthy) kept as three distinct fields, priority,
running/startup power, installer-rated nominal voltage/current/power,
schedule, health, and the last Best-First rejection reason. The nominal
electrical fields are planning estimates, not live per-load measurements:
the only INA219 is on Central's battery bus. `mode` itself is never used as a
proxy for `targetOn` here — see `Load.h`'s own class-level documentation
for why the two must stay independent.

A Node is reported `"online": true` when it has reported within
`onlineTimeoutMilliseconds`; Central's own entry is always online.
Recursion is bounded by the number of known Nodes, so a corrupted
Next-Hop relationship (a routing loop) can never spin forever — the same
guard `src/central/main.cpp`'s original tree printer used.

## Host build

This module depends on `CentralNodeRegistry.h`, which (through
`NodeReportPackets.h` → `EspNowCommunication.h`) requires the real
ESP-IDF/FreeRTOS headers, so — like `CentralNodeRegistry`,
`EspNowCommunication` and `ChipInfo` — it is ESP32-target-only with no
host-native test. Its actual formatting logic is plain string
concatenation with no further hardware dependency of its own.

## Boundary

Does not discover Nodes, does not receive ESP-NOW packets, does not run
Best-First Search, and does not decide relay states or publish anything
(`MqttManager` does that) — it only reads what `CentralNodeRegistry` and
each `Load` already know and formats it.

---

## CommissioningPackets


The commissioning-lifecycle wire-format structures sent between Kilowatts
ESP32 firmware over ESP-NOW — header-only, no `.cpp`, the same kind of
transport-only role `NodeReportPackets` already fills for measurement/relay
traffic.

## Responsibility

```cpp
struct IdentityReportPacket {
    std::uint8_t role;             // raw NodeRole byte
    std::uint8_t lifecycleState;   // raw NodeLifecycleState byte
    char firmwareVersion[12];
    char chipModel[16];
};

struct CommissionCommandPacket { std::uint32_t commandId; char friendlyName[20]; };
struct CommissionAckPacket { std::uint32_t commandId; std::uint8_t success; std::uint8_t resultingState; /* raw NodeLifecycleState byte */ };
struct DecommissionCommandPacket { std::uint32_t commandId; };
struct DecommissionAckPacket { std::uint32_t commandId; std::uint8_t success; };
```

Kept in their own file rather than added to `NodeReportPackets.h`'s
`NodeReportPacket`: that struct is already close to
`EspNowCommunication::MAX_PAYLOAD_SIZE` (it carries up to
`MAX_LOADS_PER_NODE_PACKET` `LoadReportPacket` entries) and is sent on a hot
~2s cycle, while commissioning packets are small, sent rarely, and belong to
a distinct concern — keeping them separate avoids risking the already-tested
`NodeReportPacket` layout for an unrelated change.

`IdentityReportPacket` is sent by a Node toward Central on a slow
(diagnostics-scale) cadence and immediately after any local lifecycle
change — deliberately not on `NodeReportPacket`'s hot cycle. Central's
`NodeCommissioningRegistry::recordDiscovered()` consumes it.
`CommissionCommandPacket`/`CommissionAckPacket` and
`DecommissionCommandPacket`/`DecommissionAckPacket` follow the exact
`commandId`-echo convention `RelayCommandPacket`/
`RelayCommandAcknowledgementPacket` already use, so a delayed/duplicate
acknowledgement can always be matched to the command that produced it.

As with every packet in `NodeReportPackets.h`, the owning Node is always the
surrounding `EspNowCommunication::Message` header's origin/destination MAC
address — never repeated inside these structs. `NodeRole`/
`NodeLifecycleState` come from `NodeLifecycle.h`.

`static_assert`s enforce, at compile time, that every structure here is
trivially copyable and fits inside `EspNowCommunication::MAX_PAYLOAD_SIZE`,
matching `NodeReportPackets.h`'s own pattern exactly.

## Host build

Depends on `EspNowCommunication.h`, which requires the real ESP-IDF/FreeRTOS
headers, so — like `NodeReportPackets` — this module is ESP32-target-only
with no host-native test.

## Boundaries

This header defines byte layout only. It does not perform the actual
send/receive (`EspNowCommunication`), does not decide *when* a commissioning
command should be sent (`NodeCommissioningRegistry`/`NodeIdentityStore`),
and does not validate a proposed friendly name — it only describes what
those decisions travel as over the radio.

---

## CentralConfigurationStore

Persists the Central Node's own configuration (safety policy, battery
sensor selection) to NVS. No README fragment existed before this merge.

---

## NodeLoadHardwareStore

Persists a Node's own locally-attached Load/relay hardware configuration to
NVS — installer-entered nominal ratings and relay wiring facts, not a
live measurement. See "Folder layout" above: this is a shared class both
Central and a Smart Node construct their own separate instance of.

