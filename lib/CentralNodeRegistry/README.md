# CentralNodeRegistry

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
