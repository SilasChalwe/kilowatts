# NodeRegistryJson

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
