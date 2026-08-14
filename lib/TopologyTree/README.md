# TopologyTree

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
`AUTO_ON` Load that this cycle's search rejected for budget/battery reasons
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
