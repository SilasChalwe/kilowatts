# Kilowatts MQTT

MQTT is only a transport for the same Central logic used by the serial console. It does not run a second planner.

Default namespace: `kilowatts/v1`

## Topics

The external MQTT API has only five topics:

| Topic | Direction | Retained | Purpose |
|---|---|---:|---|
| `status` | Central -> client | yes | `online` / `offline` |
| `state` | Central -> client | yes | Current system, load and node state |
| `command` | client -> Central | no | All load, system, configuration and simulation commands |
| `ack` | Central -> client | no | Command result |
| `alert` | Central -> client | no | Important state changes and monitoring warnings |

Full example topics:

```text
kilowatts/v1/status
kilowatts/v1/state
kilowatts/v1/command
kilowatts/v1/ack
kilowatts/v1/alert
```

## State

`state` combines the useful retained views into one payload:

```json
{
  "system": { },
  "loads": { },
  "nodes": { }
}
```

The system section reports the same canonical power values used by Central:

```text
P_budget
P_reserve
P_usable
P_fixed
P_auto_available
P_auto
P_remaining
P_measured
P_runtime
```

`P_measured` comes from the active measurement input. In hardware mode the input is INA219 voltage/current. In simulation mode the input is simulated voltage/current. Both follow the same PowerManager path.

## Command

Every command is sent to the single `command` topic and includes a `type` field.

### Load command

```json
{
  "type": "load",
  "commandId": 1,
  "nodeMac": "AA:BB:CC:DD:EE:FF",
  "relayPin": 16,
  "mode": "AUTO_ON"
}
```

### Power planning configuration

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

### Simulation

Enable simulation:

```json
{
  "type": "simulation",
  "commandId": 3,
  "action": "ENABLE"
}
```

Provide simulated input:

```json
{
  "type": "simulation",
  "commandId": 4,
  "action": "SET_VALUES",
  "values": {
    "batteryVoltageVolts": 15,
    "batteryCurrentAmps": 8,
    "stateOfChargePercent": 70
  }
}
```

The simulated instantaneous power is calculated exactly like the INA219 path:

```text
P_measured = voltage * current
```

Disable simulation to return to INA219 input:

```json
{
  "type": "simulation",
  "commandId": 5,
  "action": "DISABLE"
}
```

## Acknowledgements

Every accepted or rejected command is reported on `ack`.

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

`alert` is used only for meaningful changes such as:

- battery SoC reaching the configured reserve policy;
- requested runtime becoming unachievable;
- `P_measured` exceeding configured `P_budget`;
- a Smart Node going offline;
- informational configuration events.

These are software monitoring/control messages. They are not claims of hardware electrical protection.
