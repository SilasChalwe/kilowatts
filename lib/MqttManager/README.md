# MqttManager

MQTT is a transport for the same Kilowatts command interface used by the serial Console. MQTT parses JSON and the Console parses text, but both feed the shared request models in `lib/NodeManager/SystemCommandModel.h` and the same Central runtime handlers.

## Topics

Under the configured namespace (default `kilowatts/v1`):

- `state/system`
- `state/tree`
- `state/loads`
- `state/nodes`
- `config/nodes`
- `events`
- `commands/load`
- `commands/system`
- `commands/config`
- `commands/simulation`
- `acks`

The current MQTT schema version is `3`.

## Shared commands

`commands/load` updates priority, mode and/or schedule on an existing Load.

`commands/config` supports:

- `COMMISSION_NODE`
- `RENAME_NODE`
- `DECOMMISSION_NODE`
- `CONFIGURE_LOAD`
- `REMOVE_LOAD`
- `CONFIGURE_BATTERY_SENSOR`
- `CONFIGURE_POWER_LIMITS`

`commands/system` supports:

- `REQUEST_OPTIMIZATION_CYCLE`
- `FACTORY_RESET_CENTRAL`
- `FACTORY_RESET_NODE`
- `REBOOT_CENTRAL`
- `SET_OPTIMIZER_INTERVAL`
- `REPORT_OPTIMIZER_INTERVAL`

`commands/simulation` supports:

- `ENABLE`
- `DISABLE`
- `SET_VALUES`

For `SET_VALUES`, `values` may contain `batteryVoltageVolts` together with `batteryCurrentAmps`, `stateOfChargePercent`, or both groups. Voltage/current must be supplied together.

## Load configuration

The current Load configuration JSON contains only the current model:

```json
{
  "commandId": 10,
  "action": "CONFIGURE_LOAD",
  "nodeMac": "AA:BB:CC:DD:EE:FF",
  "load": {
    "name": "Fan",
    "relayPin": 16,
    "relayActiveHigh": false,
    "mode": "AUTO_OFF",
    "priority": 10,
    "powerType": "AC",
    "powerRatingWatts": 25.0,
    "schedule": null
  }
}
```

`relayPin` is the current protocol/API field name for the GPIO control pin. It does not imply that the firmware can sense a physical relay.

## System state

`state/system` reports only current fields. It does not publish deleted power-policy placeholders or fake relay/physical confirmation state. Power-flow data includes battery/main electrical limits, fixed load power, power budget, automatic load power and remaining power.

Wi-Fi and MQTT broker credential bootstrap remains Console-side because MQTT cannot be used before network/broker connectivity exists.
