# MqttManager — Kilowatts MQTT API Reference

MQTT is a transport for the same Kilowatts command interface used by the serial Console. MQTT parses JSON and the Console parses text, but both feed the shared request models in `lib/NodeManager/SystemCommandModel.h` and the same Central runtime handlers — nothing described here is a separate code path from what the Console does.

This document is written to be complete enough that a mobile/web app developer never needs to read the firmware source: every topic, every JSON field, every validation rule and every ack outcome is listed. The current MQTT schema version is `3` (embedded as `"schemaVersion"` in every published JSON payload).

## Connection

- Broker host/port/TLS/username/password are provisioned once (Console `mqtt set ...` or `commands/config` `CONFIGURE_...` is not applicable here — credentials are bootstrapped before MQTT exists, see "Bootstrapping" below) and persisted in NVS.
- TLS uses the standard ESP-IDF CA certificate bundle (`esp_crt_bundle_attach`) — there is no certificate pinning and no client certificate (mTLS). This is a known limitation, not an oversight; see `LIMITATIONS.md`.
- MQTT client ID is the fixed device ID (`central-01` by default, `CentralNodeConfig::MQTT_DEVICE_ID`) — there is exactly one Central per topic namespace.
- **Bootstrapping**: Wi-Fi and MQTT broker credentials must be set over the serial Console before any MQTT traffic is possible — Central cannot receive MQTT commands to configure its own MQTT connection. See `USER_MANUAL.md` for the `wifi set`/`mqtt set` console commands.

## Topic namespace

Every topic is `<namespace>/<suffix>` — a literal string concatenation, no device ID segment. The namespace defaults to `kilowatts/v1` and is a compile-time constant (overridable per-deployment, e.g. `kilowatts/v1/home-42`). Examples below assume the default namespace.

| Suffix | Direction | Retained | QoS | Purpose |
|---|---|---|---|---|
| `status` | published | yes | 1 | Availability (`online`/`offline`) |
| `state/system` | published | yes | 0 | Battery/power/connectivity snapshot |
| `state/tree` | published | yes | 0 | Node topology as a nested tree |
| `state/loads` | published | yes | 0 | Flat list of every Load system-wide |
| `state/nodes` | published | yes | 0 | Node registry, commissioning + online state |
| `config/nodes` | published | yes | 0 | Minimal node list for configuration UIs |
| `events` | published | no | 1 | One-shot notices (currently: Load configured/removed) |
| `alerts` | published | no | 1 | Fault/condition transitions — see below |
| `acks` | published | no | 1 | Per-command outcome |
| `commands/load` | subscribed | — | 1 | Change priority/mode/schedule on an existing Load |
| `commands/system` | subscribed | — | 1 | Central lifecycle/maintenance |
| `commands/config` | subscribed | — | 1 | Node/Load/battery installation configuration |
| `commands/simulation` | subscribed | — | 1 | Drive the simulated battery input |

All 6 retained state/status topics publish together, back-to-back, once per optimizer cycle (default every 5 minutes, configurable 5s–86400s via `SET_OPTIMIZER_INTERVAL`) and immediately after any command that changes state.

## Availability

`status` is the app-facing availability topic (full topic `kilowatts/v1/status` by default). Payload is a **bare string**, not JSON: `online` or `offline`.

When Central connects, it publishes the retained payload `online`. The MQTT client is configured with a retained Last Will payload of `offline`, so the broker publishes `offline` when it detects an unexpected disconnect. Central also refreshes `online` on every publish cycle.

The app should subscribe to this topic and record the time of the latest message. It should mark Central offline if no new `online` message arrives after two or three publish intervals. A retained `online` message must not be treated as proof that Central is online forever.

`state/system`'s `connectivity.wifiConnected`/`connectivity.mqttConnected` fields describe Central's local state at the moment that payload was built; they are not a replacement for the availability timeout above.

## Conventions

- **MAC address text**: `AA:BB:CC:DD:EE:FF` — uppercase hex, colon-separated, everywhere (published and accepted). Input parsing is case-insensitive; output is always uppercase.
- **Command field names are case-sensitive** exact matches (JSON object keys looked up case-sensitively).
- **Non-finite numbers** (NaN/Inf) in published JSON are emitted as `null`, never a bogus number.
- **`relayPin`** is the protocol/API field name for the GPIO control pin. It does not imply the firmware can sense a physical relay, appliance, or anything downstream of that pin — the firmware only knows what it commanded the pin to do.

---

## Published topics

### `state/system`

```json
{
  "schemaVersion": 3,
  "battery": {
    "sensorConfigured": true,
    "nominalVoltageVolts": 12.6,
    "capacityAmpHours": 50.0,
    "ratedEnergyWattHours": 630.0,
    "storedEnergyWattHours": 472.5,
    "usableEnergyWattHours": 346.5,
    "voltageVolts": 12.6,
    "currentAmps": 1.5,
    "currentBatteryOutputPowerWatts": 18.9,
    "measurementSource": "SIMULATED",
    "stateOfChargePercent": 75.0,
    "stateOfChargeValid": true,
    "stateOfChargeSource": "COULOMB_COUNTING",
    "batteryReserveReached": false,
    "requiredRuntimeConfigured": true,
    "requiredRuntimeHours": 24.0,
    "remainingRuntimeHours": 23.9,
    "estimatedRuntimeHours": 18.3,
    "runtimeEstimateValid": true,
    "maximumPowerForRequiredRuntimeWatts": 14.4,
    "requiredRuntimeAchievable": true
  },
  "powerFlow": {
    "batteryMaximumPowerWatts": 126.0,
    "mainMaximumPowerWatts": 189.0,
    "fixedOnPowerWatts": 8.0,
    "automaticPowerBudgetWatts": 6.43,
    "selectedAutoLoadPowerWatts": 6.0,
    "remainingAutomaticBudgetWatts": 0.43
  },
  "connectivity": {
    "wifiConnected": true,
    "wifiState": "CONNECTED",
    "mqttConnected": true
  },
  "time": {
    "valid": true,
    "source": "NTP",
    "lastOptimizationEpochSeconds": 1735689600
  },
  "diagnostics": {
    "pinCommandErrorCount": 0
  }
}
```

All numeric fields print to 3 decimal places, or `null` if not finite/not currently valid (e.g. no battery configured yet, no simulated values fed).

### `state/tree`

Nested topology, Central at the root with Smart Nodes as `children` (recursively — a Smart Node relaying through another Smart Node nests further):

```json
{
  "schemaVersion": 3,
  "central": {
    "type": "node",
    "nodeRole": "CENTRAL",
    "name": "Central",
    "mac": "A4:CF:12:0E:32:C0",
    "online": true,
    "diagnostics": {
      "firmwareVersion": "1.0.0",
      "chipModel": "ESP32-D0WDQ6",
      "freeHeapBytes": 180000,
      "minFreeHeapBytes": 150000,
      "flashSizeBytes": 4194304,
      "psramSizeBytes": 0,
      "cpuCores": 2,
      "resetReason": "SW_CPU_RESET"
    },
    "loads": [ /* Load object, see state/loads below */ ],
    "children": [
      {
        "type": "node",
        "nodeRole": "SMART",
        "name": "Kitchen",
        "mac": "AA:BB:CC:DD:EE:FF",
        "parentMac": "A4:CF:12:0E:32:C0",
        "hopCountToCentral": 1,
        "online": true,
        "diagnostics": { "...": "same shape as above" },
        "loads": [ "..." ],
        "children": []
      }
    ]
  }
}
```

`central` is `null` if no planning data exists yet. `diagnostics` is `null` if no commissioning record was found for that node.

### `state/loads`

Flat list of every Load, Central's own and every Smart Node's, in one array:

```json
{
  "schemaVersion": 3,
  "loads": [
    {
      "name": "Fridge",
      "nodeName": "Central",
      "nodeMac": "A4:CF:12:0E:32:C0",
      "relayPin": 4,
      "controlMode": "FIXED",
      "mode": "FIXED_ON",
      "manualControlAllowed": false,
      "priority": 1,
      "powerRatingWatts": 8.0,
      "powerType": "AC",
      "schedule": { "enabled": false, "startHour": 0, "startMinute": 0, "endHour": 0, "endMinute": 0 },
      "bestFirstRejectionReason": "NONE"
    }
  ]
}
```

`mode` is one of `FIXED_OFF`/`FIXED_ON`/`AUTO_OFF`/`AUTO_ON`. For an `AUTO_*` Load, the mode reflects the **result of the last Best-First cycle**, not a command — it is re-evaluated every cycle. `bestFirstRejectionReason` is one of `NONE`, `LOW_BATTERY`, `POWER_BUDGET_EXCEEDED`, `BATTERY_CURRENT_LIMIT`, `MAIN_LIMIT_EXCEEDED`, `BRANCH_LIMIT_EXCEEDED`, `UNKNOWN`.

### `state/nodes`

```json
{
  "schemaVersion": 3,
  "nodeCount": 2,
  "onlineNodeCount": 2,
  "nodes": [
    {
      "mac": "A4:CF:12:0E:32:C0",
      "role": "central",
      "nodeName": "Central",
      "lifecycleState": "commissioned",
      "syncState": "SYNCED",
      "firmwareVersion": "1.0.0",
      "chipModel": "ESP32-D0WDQ6",
      "loadCount": 8,
      "usedRelayPins": [4, 5, 18, 19, 23, 25, 26, 27],
      "availableRelayPins": [21, 22],
      "diagnostics": {
        "freeHeapBytes": 180000, "minFreeHeapBytes": 150000,
        "flashSizeBytes": 4194304, "psramSizeBytes": 0,
        "siliconRevision": 1, "cpuCores": 2, "cpuFrequencyMhz": 240,
        "resetReason": "SW_CPU_RESET",
        "temperatureAvailable": false, "temperatureCelsius": null
      },
      "online": true,
      "hopCountToCentral": 0,
      "nextHopMac": null
    }
  ]
}
```

`role` is `central`/`smart`/`unknown`. `lifecycleState` is one of `factory`, `uncommissioned`, `discovered`, `configuring`, `commissioned`, `operational`, `decommissioned`, `unknown`. A node not currently in the live topology reports `"online": false, "hopCountToCentral": null, "nextHopMac": null`; Central always reports `"online": true, "hopCountToCentral": 0, "nextHopMac": null`. A node counts online if last heard from within 10 seconds.

### `config/nodes`

The minimal subset of `state/nodes` needed to drive a configuration UI — no diagnostics, online status, or sync state:

```json
{
  "schemaVersion": 3,
  "nodeCount": 2,
  "nodes": [
    {
      "mac": "A4:CF:12:0E:32:C0",
      "role": "central",
      "nodeName": "Central",
      "lifecycleState": "commissioned",
      "loadCount": 8,
      "usedRelayPins": [4, 5, 18, 19, 23, 25, 26, 27],
      "availableRelayPins": [21, 22]
    }
  ]
}
```

### `events`

Not retained. One-shot notice, currently fired only for a successful Load configure/remove on a Smart Node (via ESP-NOW ack):

```json
{ "schemaVersion": 3, "eventType": "LOAD_CONFIGURED", "target": "AA:BB:CC:DD:EE:FF", "detail": null }
```

`eventType` is `"LOAD_CONFIGURED"` or `"LOAD_REMOVED"`. `target` is the Smart Node's MAC. `detail` is always `null` today.

### `alerts`

Not retained. Pushed **only on a state transition** — never repeated every cycle while a condition holds. This is the mechanism to watch for real-time notification (a dashboard app should not rely on polling `state/system` for this):

```json
{ "schemaVersion": 3, "alertType": "BATTERY_RESERVE", "severity": "critical", "detail": "battery state of charge reached the configured reserve" }
```

`severity` is a free-form string, not a fixed enum (new severities can be added without a schema change). Observed values: `"info"`, `"warning"`, `"critical"`.

| `alertType` | Fires when | `severity` / `detail` |
|---|---|---|
| `BATTERY_RESERVE` | SoC crosses the configured reserve threshold | `critical` / "reached the configured reserve" — `info` / "recovered above the configured reserve" |
| `RUNTIME_TARGET` | The configured required-runtime target becomes achievable/unachievable | `warning` / "no longer achievable at the current load" — `info` / "achievable again" |
| `BATTERY_SENSOR` | The real INA219 stops/resumes responding (hardware mode only — never fires while simulating) | `critical` / "stopped responding" — `info` / "reading resumed" |
| `NODE_OFFLINE` | A Smart Node's online status flips | `warning` / "stopped reporting and is now offline" — `info` / "came back online" |

### `acks`

Not retained. Exactly one synchronous ack per inbound command, published immediately:

```json
{ "schemaVersion": 3, "commandId": 42, "commandType": "CONFIGURE_LOAD", "status": "APPLIED", "reason": "Central Load configured", "target": "A4:CF:12:0E:32:C0" }
```

`status` is one of:
- **`REJECTED`** — the request was invalid or refused synchronously (bad JSON, missing/invalid field, business-rule failure). `reason` explains why.
- **`ACCEPTED`** — the request was valid and dispatched (typically over ESP-NOW to a Smart Node) but not yet confirmed complete.
- **`APPLIED`** — completed successfully.
- **`FAILED`** — accepted, but a later async step failed.

`commandId` is `0` if the JSON couldn't even be parsed or `commandId` itself was missing/invalid. `target` is the relevant node's MAC, or `null` for commands with no single node target.

**Two-phase acknowledgement**: any `commands/config` action that must reach a remote Smart Node over ESP-NOW (`COMMISSION_NODE`/`RENAME_NODE`, `CONFIGURE_LOAD`/`REMOVE_LOAD` when `nodeMac` isn't Central, `DECOMMISSION_NODE` when the node is reachable) plus `commands/system`'s `FACTORY_RESET_NODE`, publish a synchronous `ACCEPTED` ack immediately, then a **second, independent** `acks` message with the *same* `commandId` and `APPLIED`/`FAILED` once (and only if) the Smart Node's own ESP-NOW ack arrives. **A client must match acks by `commandId`, not assume exactly one ack per command.** If a Smart Node never replies, no second ack is ever published — a client-side timeout is the app's own responsibility.

---

## Subscribed (command) topics

Every command topic rejects unparseable JSON synchronously (`status: REJECTED`, `commandId: 0`). Field names are case-sensitive.

### `commands/load` — change priority/mode/schedule on an existing Load

No `action` field — this topic has exactly one implicit action.

| Field | Type | Required | Notes |
|---|---|---|---|
| `commandId` | integer > 0 | yes | |
| `nodeMac` | MAC string | yes | Central's own MAC, or a commissioned Smart Node's |
| `relayPin` | integer 0–255 | yes | |
| `priority` | integer 0–65535 | no | omit to leave unchanged |
| `mode` | `FIXED_OFF`\|`FIXED_ON`\|`AUTO_OFF`\|`AUTO_ON` | no | omit to leave unchanged |
| `schedule` | object, see below | no | omit to leave unchanged |

**Schedule object** (also used by `commands/config`'s `CONFIGURE_LOAD.load.schedule`):

| Field | Type | Required |
|---|---|---|
| `enabled` | bool | yes |
| `startHour` | 0–23 | yes if `enabled` |
| `startMinute` | 0–59 | yes if `enabled` |
| `endHour` | 0–23 | yes if `enabled` |
| `endMinute` | 0–59 | yes if `enabled` — start/end total-minutes must differ |

Setting `mode` to `FIXED_ON`/`FIXED_OFF` forces the schedule disabled regardless of what's sent. Any provided field overrides the Load's current value; unspecified fields are left unchanged. Always exactly one synchronous ack (`commandType: "LOAD"`), `APPLIED` or `REJECTED` — this topic never produces `ACCEPTED`/`FAILED`.

```json
{ "commandId": 10, "nodeMac": "A4:CF:12:0E:32:C0", "relayPin": 5, "priority": 8, "mode": "AUTO_OFF" }
```

### `commands/system` — Central-level lifecycle/maintenance

| Field | Type | Required |
|---|---|---|
| `commandId` | integer > 0 | yes |
| `action` | see table | yes |

| `action` | Extra fields | Behavior |
|---|---|---|
| `REQUEST_OPTIMIZATION_CYCLE` | none | Triggers an optimizer cycle immediately. Always `APPLIED`. |
| `REPORT_OPTIMIZER_INTERVAL` | none | Returns the current interval as text. Always `APPLIED`. |
| `SET_OPTIMIZER_INTERVAL` | `optimizerIntervalSeconds` (or `intervalSeconds`, same field, either name works) **or** `intervalMinutes` (≤1440) | Range 5–86400 seconds; out of range/missing → `REJECTED`. |
| `REBOOT_CENTRAL` | none | Schedules a restart 1.5s later. Ack is sent before the reboot happens. |
| `FACTORY_RESET_CENTRAL` | `confirm` = `"RESET"` or `"FACTORY_RESET_CONFIRMED"` (exact, case-sensitive) | Erases all NVS and restarts. No final ack in practice (device is restarting). |
| `FACTORY_RESET_NODE` | `targetNodeMac` | Requires a live ESP-NOW route, else `REJECTED`. Two-phase ack: `ACCEPTED` then `APPLIED`/`FAILED`. |

Anything else → `REJECTED "unsupported system command"`. `target` is always `null` for synchronous system acks, except the async `FACTORY_RESET_NODE` follow-up (carries the target node's MAC).

```json
{ "commandId": 11, "action": "SET_OPTIMIZER_INTERVAL", "optimizerIntervalSeconds": 300 }
```

### `commands/config` — installation/configuration commands

Base fields, required for every action:

| Field | Type | Required |
|---|---|---|
| `commandId` | integer > 0 | yes |
| `action` | see table | yes |
| `nodeMac` | MAC string | yes — the target node |

Once `action` parses, every ack's `commandType` = the literal action string (e.g. `"CONFIGURE_LOAD"`).

| `action` | Extra fields | Behavior |
|---|---|---|
| `COMMISSION_NODE` | `friendlyName` (string, 1–19 chars) | Target must be a discovered, non-Central node with a live route. Two-phase ack. |
| `RENAME_NODE` | `friendlyName` | Identical code path to `COMMISSION_NODE` — the firmware does not distinguish a rename from commissioning-with-a-new-name; the async follow-up ack is always labeled `"COMMISSION_NODE"`. |
| `DECOMMISSION_NODE` | none | Can't target Central. Decommissions locally first; if a route exists also dispatches ESP-NOW and two-phase-acks (`ACCEPTED` then `APPLIED`/`FAILED`); if the node is already offline, decommissions immediately (`APPLIED`, no follow-up). |
| `CONFIGURE_LOAD` | nested `load` object — see below | Local (Central) target is synchronous `APPLIED`/`REJECTED`. Smart Node target requires commissioning + route, two-phase-acks, and on success also publishes `events` `LOAD_CONFIGURED`. |
| `REMOVE_LOAD` | `relayPin` (top-level, not nested) | Same local-vs-Smart-Node split as `CONFIGURE_LOAD`; publishes `events` `LOAD_REMOVED` on success. |
| `CONFIGURE_BATTERY_SENSOR` | nested `batterySensor` object — see below | Central-only (`nodeMac` must be Central's own). |
| `CONFIGURE_POWER_LIMITS` | nested `powerLimits` object — see below | Central-only. |

**`CONFIGURE_LOAD.load` object** — all fields required:

| Field | Type | Notes |
|---|---|---|
| `name` | string, 1–15 chars | |
| `relayPin` | integer 0–255 | |
| `relayActiveHigh` | bool | |
| `mode` | `FIXED_OFF`\|`FIXED_ON`\|`AUTO_OFF`\|`AUTO_ON` | |
| `powerType` | `AC`\|`DC` | |
| `priority` | integer 0–65535 | |
| `powerRatingWatts` | number ≥ 0 | |
| `schedule` | object (§Schedule above) | **required** here, unlike `commands/load` where it's optional |

**`CONFIGURE_BATTERY_SENSOR.batterySensor` object** — all 6 fields required; these describe an installation's real INA219 hardware, never invented/defaulted by the firmware:

| Field | Type | Valid range |
|---|---|---|
| `shuntResistanceOhms` | float | finite, > 0 |
| `maximumExpectedCurrentAmps` | float | finite, > 0 |
| `emaAlpha` | float | finite, 0 < x ≤ 1 (smoothing factor) |
| `batteryCapacityAmpHours` | float | finite, > 0 |
| `initialStateOfChargePercent` | float | 0–100 |
| `nominalVoltageVolts` | float | finite, > 0 |

Note: `shuntResistanceOhms × maximumExpectedCurrentAmps` must not exceed 0.32 V (the INA219's shunt-voltage measurement range) — an out-of-range pair is rejected by `PowerManager::initialize()`, not by this command's own validation, so it currently surfaces as a generic apply failure rather than a specific reason string.

**`CONFIGURE_POWER_LIMITS.powerLimits` object** — all 4 fields required; this is the installation's reserve/runtime policy, exactly what simulation mode exists to help decide before committing to real numbers:

| Field | Type | Valid range |
|---|---|---|
| `minimumStateOfChargePercent` | float | 0–100 |
| `maximumBatteryDischargeCurrentAmps` | float | finite, > 0 |
| `maximumMainCurrentAmps` | float | finite, > 0 |
| `requiredRuntimeHours` | float | ≥ 0 (`0` = no runtime target) |

```json
{
  "commandId": 12, "action": "CONFIGURE_LOAD", "nodeMac": "A4:CF:12:0E:32:C0",
  "load": {
    "name": "Fan", "relayPin": 19, "relayActiveHigh": false,
    "mode": "AUTO_OFF", "priority": 3, "powerType": "DC", "powerRatingWatts": 5.0,
    "schedule": { "enabled": false, "startHour": 0, "startMinute": 0, "endHour": 0, "endMinute": 0 }
  }
}
```

### `commands/simulation` — drive the simulated battery input

| Field | Type | Required |
|---|---|---|
| `commandId` | integer > 0 | yes |
| `action` | `ENABLE`\|`DISABLE`\|`SET_VALUES` | yes |

| `action` | Extra fields | Behavior |
|---|---|---|
| `ENABLE` | none | Switches the battery monitor to simulated input. Works immediately — no prior `CONFIGURE_BATTERY_SENSOR`/`CONFIGURE_POWER_LIMITS` required. `APPLIED` if the monitor is ready, `ACCEPTED` otherwise (reason explains why). |
| `DISABLE` | none | Switches to the real INA219 input. Correctly requires real installer-entered sensor values to reach `APPLIED` — reports why it isn't ready otherwise (not responding, or never configured). |
| `SET_VALUES` | nested `values` object | See below. Requires simulation already `ENABLE`d. |

**`SET_VALUES.values` object** — every field optional individually, but constrained together:

| Field | Type | Notes |
|---|---|---|
| `batteryVoltageVolts` | number | Must be paired with `batteryCurrentAmps` — one without the other is rejected |
| `batteryCurrentAmps` | number | See above |
| `stateOfChargePercent` | number | Independent of the voltage/current pair |

At least one of {voltage+current pair, `stateOfChargePercent`} must be present. `target` is always `null` for this topic (no node MAC concept applies).

```json
{ "commandId": 13, "action": "SET_VALUES", "values": { "batteryVoltageVolts": 12.6, "batteryCurrentAmps": 1.5, "stateOfChargePercent": 75 } }
```
