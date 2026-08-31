# Kilowatts MQTT

MQTT is only a transport for the same Central logic used by the serial console. It does not run another planner.

Default namespace:

```text
kilowatts/v1
```

## Topics

Only five external topics are used:

| Topic | Direction | Purpose |
|---|---|---|
| `status` | Central -> client | online/offline |
| `state` | Central -> client | current system, loads and nodes |
| `command` | client -> Central | all commands |
| `ack` | Central -> client | command result |
| `alert` | Central -> client | important monitoring/state changes |

Full topics:

```text
kilowatts/v1/status
kilowatts/v1/state
kilowatts/v1/command
kilowatts/v1/ack
kilowatts/v1/alert
```

## State

`state` combines the useful retained data:

```json
{
  "system": {},
  "loads": {},
  "nodes": {}
}
```

The power-flow section uses only:

```text
P_budget
P_reserve
P_fixed
P_auto_available
P_auto
P_remaining
```

Battery measurement contains:

```text
P_measured
```

Runtime is reported with fields such as required hours, remaining hours, estimated hours, and achievable status. Its internal power calculation is folded directly into `P_auto_available`.

## Command

Every MQTT command goes to `command` and includes a `type`.

### Power planning

```json
{
  "type": "config",
  "commandId": 2,
  "action": "CONFIGURE_POWER_PLANNING",
  "nodeMac": "AA:BB:CC:DD:EE:FF",
  "powerPlanning": {
    "P_budget": 200,
    "P_reserve": 20,
    "minimumStateOfChargePercent": 20,
    "requiredRuntimeHours": 24
  }
}
```

### Load command

```json
{
  "type": "load",
  "commandId": 3,
  "nodeMac": "AA:BB:CC:DD:EE:FF",
  "relayPin": 16,
  "mode": "AUTO_ON"
}
```

### Simulation input

Enable:

```json
{
  "type": "simulation",
  "commandId": 4,
  "action": "ENABLE"
}
```

Set measurement values:

```json
{
  "type": "simulation",
  "commandId": 5,
  "action": "SET_VALUES",
  "values": {
    "batteryVoltageVolts": 15,
    "batteryCurrentAmps": 8,
    "stateOfChargePercent": 70
  }
}
```

Simulation calculates power exactly like INA219:

```text
P_measured = voltage × current
```

Disable simulation and return to INA219:

```json
{
  "type": "simulation",
  "commandId": 6,
  "action": "DISABLE"
}
```

## Acknowledgement

Results are published on `ack`:

```json
{
  "schemaVersion": 4,
  "commandId": 2,
  "commandType": "CONFIGURE_POWER_PLANNING",
  "status": "APPLIED",
  "reason": "power planning configured",
  "target": "AA:BB:CC:DD:EE:FF"
}
```

## Alerts

`alert` is used for meaningful changes such as:

- battery SoC reaching the configured reserve policy;
- requested runtime becoming unachievable;
- `P_measured` exceeding `P_budget`;
- a Smart Node going offline.

These are monitoring/control messages, not hardware electrical protection claims.
