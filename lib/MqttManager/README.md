# Kilowatts MQTT

MQTT is the frontend transport for Central operations. Wi-Fi and MQTT broker setup are local installation settings and are configured only from the Central serial console.

Default namespace:

```text
kilowatts/v1
```

## Five topics

```text
kilowatts/v1/status
kilowatts/v1/state
kilowatts/v1/command
kilowatts/v1/ack
kilowatts/v1/alert
```

| Topic | Direction | Purpose |
|---|---|---|
| `status` | Central -> frontend | Central presence: `online` or `offline` |
| `state` | Central -> frontend | system, loads and nodes |
| `command` | frontend -> Central | operational commands |
| `ack` | Central -> frontend | command result |
| `alert` | Central -> frontend | important changes |

## Central presence

The frontend must use `status` to decide whether the Central Node is alive.

Central publishes retained:

```text
online
```

The MQTT Last Will is retained as:

```text
offline
```

This avoids the common mistake of showing "system connected" only because the browser/frontend itself is connected to the broker.

The Central entry inside `state.nodes` therefore does not provide its own authoritative online flag. Smart Node online/offline state is still reported from their actual reports to Central.

## State

`state` has three useful parts:

```json
{
  "system": {},
  "loads": {},
  "nodes": {}
}
```

The `system` section contains battery, measurement and power-flow information. It does not publish Wi-Fi state, MQTT connection state or broker credentials.

Power names are:

```text
P_budget
P_reserve
P_fixed
P_auto_available
P_auto
P_remaining
P_measured
```

The `nodes` section keeps useful device information for Central and Smart Nodes, including:

```text
mac
role
nodeName
lifecycleState
syncState
firmwareVersion
chipModel
loadCount
usedRelayPins
availableRelayPins
diagnostics
```

Diagnostics include heap, flash, PSRAM, silicon revision, CPU cores/frequency, reset reason and temperature when available.

## Commands

Every command is sent to `kilowatts/v1/command`.

Only these frontend command types are accepted:

```text
node
load
battery
sensor
system
```

There is no frontend `wifi`, `mqtt`, `config` or `simulation` command type.

### Node

Add/commission:

```json
{
  "type": "node",
  "commandId": 1,
  "action": "add",
  "mac": "AA:BB:CC:DD:EE:01",
  "name": "Room-1"
}
```

Rename/update:

```json
{
  "type": "node",
  "commandId": 2,
  "action": "update",
  "mac": "AA:BB:CC:DD:EE:01",
  "name": "Lab-1"
}
```

Delete/decommission:

```json
{
  "type": "node",
  "commandId": 3,
  "action": "delete",
  "mac": "AA:BB:CC:DD:EE:01"
}
```

### Load

Add:

```json
{
  "type": "load",
  "commandId": 10,
  "action": "add",
  "nodeMac": "AA:BB:CC:DD:EE:01",
  "relayPin": 16,
  "name": "Fan",
  "power": 40,
  "priority": 2,
  "powerType": "DC",
  "activeHigh": true,
  "mode": "AUTO_OFF"
}
```

Update mode/priority/schedule:

```json
{
  "type": "load",
  "commandId": 11,
  "action": "update",
  "nodeMac": "AA:BB:CC:DD:EE:01",
  "relayPin": 16,
  "mode": "AUTO_ON"
}
```

Delete:

```json
{
  "type": "load",
  "commandId": 12,
  "action": "delete",
  "nodeMac": "AA:BB:CC:DD:EE:01",
  "relayPin": 16
}
```

### Battery planning

```json
{
  "type": "battery",
  "commandId": 20,
  "action": "set",
  "budget": 200,
  "reserve": 20,
  "minSoc": 20,
  "runtime": 24
}
```

Runtime is optional. It affects `P_auto_available`; it does not create another public power variable.

### Sensor

Use INA219:

```json
{
  "type": "sensor",
  "commandId": 30,
  "action": "ina219"
}
```

Use simulation:

```json
{
  "type": "sensor",
  "commandId": 31,
  "action": "sim"
}
```

Set simulated measurements:

```json
{
  "type": "sensor",
  "commandId": 32,
  "action": "values",
  "voltage": 15,
  "current": 8,
  "soc": 70
}
```

Both sources feed the same calculation:

```text
P_measured = voltage * current
```

INA219 technical/shunt configuration is installation setup and stays on the Central console.

### System

Run planning now:

```json
{
  "type": "system",
  "commandId": 40,
  "action": "optimize"
}
```

Change automatic planning interval:

```json
{
  "type": "system",
  "commandId": 41,
  "action": "interval",
  "seconds": 300
}
```

Restart Central:

```json
{
  "type": "system",
  "commandId": 42,
  "action": "restart"
}
```

## Acknowledgement

Example:

```json
{
  "schemaVersion": 4,
  "commandId": 20,
  "commandType": "battery",
  "status": "APPLIED",
  "reason": "power planning configured",
  "target": null
}
```

## Alerts

`alert` is for meaningful changes such as:

- battery SoC reaching the configured reserve policy;
- requested runtime becoming unachievable;
- `P_measured` exceeding `P_budget`;
- a Smart Node going offline or returning online.

These are monitoring/control messages, not hardware electrical protection.
