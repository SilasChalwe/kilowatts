# MqttManager — Kilowatts MQTT API Reference

MQTT and the serial Console are two transports for the same Central command handlers. MQTT does not contain a second power-planning implementation.

Current MQTT schema version: **4**.

## Topic namespace

Every topic is `<namespace>/<suffix>`. The default namespace is `kilowatts/v1`.

| Suffix | Direction | Retained | Purpose |
|---|---|---:|---|
| `status` | Central -> broker | yes | `online` / `offline` availability |
| `state/system` | Central -> broker | yes | Battery measurement, power flow and runtime state |
| `state/tree` | Central -> broker | yes | Node topology |
| `state/loads` | Central -> broker | yes | All configured Loads |
| `state/nodes` | Central -> broker | yes | Node registry and online state |
| `config/nodes` | Central -> broker | yes | Node configuration view |
| `events` | Central -> broker | no | One-shot system events |
| `alerts` | Central -> broker | no | State-transition warnings/information |
| `acks` | Central -> broker | no | Command results |
| `commands/load` | broker -> Central | no | Change an existing Load |
| `commands/system` | broker -> Central | no | System commands |
| `commands/config` | broker -> Central | no | Installation/configuration commands |
| `commands/simulation` | broker -> Central | no | Select/feed simulated battery input |

## Power model

The configured installation budget is the planning source of truth.

```text
P_usable = P_budget - P_reserve

P_auto_available = planningAllowance - P_fixed

P_remaining = P_budget - (P_fixed + P_auto)
```

When no runtime target is active:

```text
planningAllowance = P_usable
```

When a runtime target is active:

```text
usableEnergyWh =
    batteryCapacityAh * nominalVoltageVolts *
    max(0, SoC - minimumSoC) / 100

P_runtime = usableEnergyWh / remainingRuntimeHours

planningAllowance = min(P_usable, P_runtime)
```

The INA219 and simulation do **not** create `P_budget`. They supply voltage and current to the same measurement path:

```text
P_measured = voltage * current
```

`P_measured` is monitoring data. It is not the unused or maximum power capacity of the battery.

## `state/system`

Example:

```json
{
  "schemaVersion": 4,
  "battery": {
    "sensorConfigured": true,
    "nominalVoltageVolts": 15.0,
    "capacityAmpHours": 200.0,
    "ratedEnergyWattHours": 3000.0,
    "storedEnergyWattHours": 2100.0,
    "usableEnergyWattHours": 1500.0,
    "voltageVolts": 15.0,
    "currentAmps": 4.1,
    "P_measured": 61.5,
    "measurementSource": "HARDWARE",
    "stateOfChargePercent": 70.0,
    "stateOfChargeValid": true,
    "stateOfChargeSource": "COULOMB_COUNTING",
    "batteryReserveReached": false,
    "requiredRuntimeConfigured": true,
    "requiredRuntimeHours": 24.0,
    "remainingRuntimeHours": 24.0,
    "estimatedRuntimeHours": 24.39,
    "runtimeEstimateValid": true,
    "P_runtime": 62.5,
    "requiredRuntimeAchievable": true
  },
  "powerFlow": {
    "P_budget": 200.0,
    "P_reserve": 20.0,
    "P_usable": 180.0,
    "P_fixed": 40.0,
    "P_auto_available": 22.5,
    "P_auto": 20.0,
    "P_remaining": 140.0
  },
  "connectivity": {
    "wifiConnected": true,
    "wifiState": "CONNECTED",
    "mqttConnected": true
  },
  "time": {
    "valid": true,
    "source": "NTP",
    "lastOptimizationEpochSeconds": 0
  },
  "diagnostics": {
    "pinCommandErrorCount": 0
  }
}
```

### Power-field meanings

| Field | Meaning |
|---|---|
| `P_budget` | Configured installation power that the system is allowed to allocate |
| `P_reserve` | Watts deliberately kept outside normal load allocation |
| `P_usable` | `P_budget - P_reserve` |
| `P_fixed` | Sum of all currently FIXED_ON Load ratings |
| `P_auto_available` | Power passed to Best-First Search for AUTO Loads |
| `P_auto` | Sum of AUTO Loads selected by Best-First Search |
| `P_remaining` | `P_budget - (P_fixed + P_auto)` |
| `P_measured` | Instantaneous measured/simulated `V * I` |
| `P_runtime` | Average power allowed by the requested battery runtime target |

`P_remaining` includes both intentionally reserved power and any allocatable power that Best-First did not use.

## Configure power planning

Publish to `commands/config`:

```json
{
  "commandId": 1001,
  "action": "CONFIGURE_POWER_PLANNING",
  "nodeMac": "AA:BB:CC:DD:EE:FF",
  "powerPlanning": {
    "P_budget": 200.0,
    "P_reserve": 20.0,
    "minimumStateOfChargePercent": 20.0,
    "requiredRuntimeHours": 24.0
  }
}
```

Rules:

- `P_budget > 0`
- `P_reserve >= 0`
- `P_reserve <= P_budget`
- `minimumStateOfChargePercent` is `0..100`
- `requiredRuntimeHours >= 0`
- `requiredRuntimeHours = 0` disables runtime-based allocation

The power-planning command targets the Central node only.

## Configure battery measurement/energy metadata

Publish to `commands/config`:

```json
{
  "commandId": 1002,
  "action": "CONFIGURE_BATTERY_SENSOR",
  "nodeMac": "AA:BB:CC:DD:EE:FF",
  "batterySensor": {
    "shuntResistanceOhms": 0.1,
    "maximumExpectedCurrentAmps": 3.0,
    "emaAlpha": 0.5,
    "batteryCapacityAmpHours": 200.0,
    "initialStateOfChargePercent": 70.0,
    "nominalVoltageVolts": 15.0
  }
}
```

`maximumExpectedCurrentAmps` is an INA219 measurement-range configuration value. It is **not** a software over-current protection setting.

Battery capacity and nominal voltage are also used when a runtime target is configured.

## Simulation

Simulation and INA219 feed the same `PowerManager` measurement path. Simulation is only a different source for the input values.

Enable simulation:

```json
{
  "commandId": 2001,
  "action": "ENABLE"
}
```

Feed simulated values:

```json
{
  "commandId": 2002,
  "action": "SET_VALUES",
  "values": {
    "batteryVoltageVolts": 15.0,
    "batteryCurrentAmps": 4.1,
    "stateOfChargePercent": 70.0
  }
}
```

Disable simulation and return to INA219 input:

```json
{
  "commandId": 2003,
  "action": "DISABLE"
}
```

Voltage and current must be supplied together. SoC can be supplied with them or separately.

## Load commands

`commands/load` changes the priority, configured mode and/or schedule of an existing Load.

Example:

```json
{
  "commandId": 3001,
  "nodeMac": "AA:BB:CC:DD:EE:FF",
  "relayPin": 4,
  "priority": 3,
  "mode": "AUTO_ON"
}
```

Valid modes are:

- `FIXED_OFF`
- `FIXED_ON`
- `AUTO_OFF`
- `AUTO_ON`

FIXED_ON demand is accounted before Best-First Search. Best-First Search receives `P_auto_available` and selects AUTO Loads that fit within that value.

## Alerts

Alerts are emitted on transitions, not continuously.

| `alertType` | Meaning |
|---|---|
| `BATTERY_RESERVE` | SoC crossed the configured minimum SoC reserve |
| `RUNTIME_TARGET` | Requested runtime became achievable/unachievable |
| `BATTERY_SENSOR` | Real INA219 stopped/resumed responding |
| `MEASURED_POWER_BUDGET` | `P_measured` crossed above/below configured `P_budget` |
| `NODE_OFFLINE` | Smart Node online state changed |

`MEASURED_POWER_BUDGET` is monitoring. It does not claim that firmware provides electrical hardware protection.

## Best-First rejection reason

The active planning result uses only:

- `NONE`
- `POWER_BUDGET_EXCEEDED`

The old battery-current, main-current and branch-current rejection reasons are not part of schema 4.

## Acknowledgements

`acks` payload:

```json
{
  "schemaVersion": 4,
  "commandId": 1001,
  "commandType": "CONFIGURE_POWER_PLANNING",
  "status": "APPLIED",
  "reason": "power planning configured",
  "target": "AA:BB:CC:DD:EE:FF"
}
```

`status` is one of `ACCEPTED`, `APPLIED`, `REJECTED`, or `FAILED`.

## Hardware-protection boundary

Kilowatts firmware performs monitoring, planning and relay/GPIO commands. It must not be described as replacing physical electrical protection. Fuses, breakers, BMS cut-offs and other correctly rated protection hardware remain separate from this software power-allocation logic.
