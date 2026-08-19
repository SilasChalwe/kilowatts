# MqttManager

Central Node MQTT connectivity and the `kilowatts/v1` topic contract —
the bridge between Central's own local state and the mobile application.

## Topics

| Topic | Publisher | Subscriber | QoS | Retained |
|---|---|---|---|---|
| `kilowatts/v1/state/system` | Central | Application | 1 | Yes |
| `kilowatts/v1/state/tree` | Central | Application | 1 | Yes |
| `kilowatts/v1/state/loads` | Central | Application | 1 | Yes |
| `kilowatts/v1/state/nodes` | Central | Application | 1 | Yes |
| `kilowatts/v1/config/nodes` | Central | Application | 1 | Yes |
| `kilowatts/v1/events` | Central | Application | 1 | No |
| `kilowatts/v1/commands/load` | Application | Central | 1 | No |
| `kilowatts/v1/commands/system` | Application | Central | 1 | No |
| `kilowatts/v1/commands/config` | Application | Central | 1 | No |
| `kilowatts/v1/acks` | Central | Application | 1 | No |

State/config topics are retained so a freshly opened mobile application
sees the last known state immediately, without waiting for the next
planning cycle. Commands, acknowledgements and events are not retained —
a stale command must never be silently replayed to a newly (re)connecting
Central, and a past event is not a current fact.

`state/nodes`/`config/nodes` are built by `NodeRegistryJson` from
`NodeCommissioningRegistry` (identity/lifecycle) joined with
`CentralNodeRegistry` (route/online, `state/nodes` only) — see that
module's own README for the state-vs-configuration split between the two
topics. `commands/config` carries commissioning lifecycle operations
(`COMMISSION_NODE`, `RENAME_NODE`, `DECOMMISSION_NODE`) and installer
configuration (`CONFIGURE_LOAD`, `CONFIGURE_BATTERY_SENSOR`). Smart-node
loads have no per-load INA219: the installer supplies nominal
voltage/current, a branch limit and startup watts; Central derives the
conservative planned running power. The only INA219 is commissioned
separately on Central's battery bus.

## Responsibility

`MqttManager` owns the broker connection and the wire contract only. It
does not compute anything: `publish()` takes a JSON string the caller
already built (`SystemStateJson::build()`, `TopologyTree::buildTreeJson()`
/`buildLoadsJson()`) and sends it to `<namespace>/<topicSuffix>`.

```cpp
MqttManager mqtt(CentralNodeConfig::MQTT_TOPIC_NAMESPACE, CentralNodeConfig::MQTT_DEVICE_ID,
                  CentralNodeConfig::MQTT_SCHEMA_VERSION);

mqtt.setLoadCommandHandler(&handleLoadCommand, &applicationContext);
mqtt.setSystemCommandHandler(&handleSystemCommand, &applicationContext);
mqtt.setConfigCommandHandler(&handleConfigCommand, &applicationContext);

// Broker choice (only call begin() after WiFiManager::isConnected()):
// an installer-provisioned broker (MqttCredentialsStore - e.g. a local
// broker on the site's own network, for when there is no reliable
// internet uplink) wins if one exists; otherwise this falls back to the
// compiled-in cloud broker in KilowattsSecrets.h.
MqttCredentialsStore::Credentials provisioned{};
if (mqttCredentialsStore.load(provisioned)) {
    mqtt.begin(MqttManager::Credentials{provisioned.host, provisioned.port, provisioned.useTls,
                                         provisioned.username, provisioned.password});
} else {
    mqtt.begin(MqttManager::Credentials{Secrets::MQTT_BROKER_HOST, Secrets::MQTT_BROKER_PORT,
                                         Secrets::MQTT_BROKER_USE_TLS, Secrets::MQTT_USERNAME,
                                         Secrets::MQTT_PASSWORD});
}

mqtt.publish(MqttManager::TOPIC_STATE_SYSTEM, systemStateJson, /* qos */ 1, /* retain */ true);
```

Incoming `commands/load`/`commands/system`/`commands/config` messages are
parsed (via cJSON) into plain typed `LoadCommandRequest`/
`SystemCommandRequest`/`ConfigCommandRequest` structs and handed to the
caller-supplied handler — `MqttManager` itself performs only syntactic
validation (well-formed JSON, a parseable MAC address, a recognised
`mode`/`action` string, an in-range schedule hour/minute *when a schedule
is present*, a non-empty `friendlyName` *when the action requires one*).
All domain validation (does this Node/Load actually exist, is the
priority within `W_max`, does the current commissioning state allow this
transition) is the handler's responsibility — see
`src/central/main.cpp`'s `handleLoadCommand()`/`handleConfigCommand()`.

Every command, accepted or rejected, receives a structured acknowledgement
on `acks`:

```json
{"schemaVersion":1,"commandId":42,"commandType":"LOAD","status":"REJECTED","reason":"priority exceeds W_max","target":"AA:BB:CC:DD:EE:FF"}
```

`status` is one of `ACCEPTED`/`APPLIED`/`REJECTED`/`FAILED` (see
`AckStatus`) rather than a bare boolean. `COMMISSION_NODE`, `RENAME_NODE`
and `CONFIGURE_LOAD` first publish `ACCEPTED`/`REJECTED`, then a **second**
ack for the same `commandId` once the addressed Smart Node confirms over
ESP-NOW. `CONFIGURE_BATTERY_SENSOR` and `DECOMMISSION_NODE` are complete
once Central durably applies its local state, so their command ack is
directly `APPLIED`/`REJECTED`; a later decommission reset ACK is diagnostic
evidence about the Node itself. `LOAD`/`SYSTEM` commands also apply
synchronously. `target` is JSON `null` when not applicable to that command.

`publishEvent()` publishes one object to `events` — `eventType` (e.g.
`"NODE_DISCOVERED"`, `"NODE_COMMISSIONED"`, `"NODE_OFFLINE"`), `target`
(`null` when not applicable) and a free-text `detail`:

```json
{"schemaVersion":1,"eventType":"NODE_DISCOVERED","target":"AA:BB:CC:DD:EE:FF","detail":null}
```

## MQTT/Wi-Fi loss

`publish()` returns `false` (does not queue, does not block) whenever
`isConnected()` is false — MQTT loss must never stop sensor monitoring,
Best-First Search, ESP-NOW or relay control. Reconnection itself is
esp-mqtt's own built-in retry/backoff; `MqttManager` does not reimplement
it, only reports `DISCONNECTED`/`CONNECTING`/`CONNECTED` honestly as
esp-mqtt's events arrive.

## Local testing

To test against a broker on your own network instead of the cloud one
(no internet required), run a broker on your dev machine and provision
its address through the captive portal's optional MQTT fields (see
`WiFiProvisioningPortal`) instead of leaving them blank:

```
sudo apt install mosquitto mosquitto-clients   # if not already installed
sudo systemctl enable --now mosquitto
hostname -I                                    # this machine's LAN IP - use it as the broker host
```

Default Mosquitto config (`/etc/mosquitto/mosquitto.conf`) listens on
port `1883`, no TLS, `allow_anonymous true` (no username/password) —
enter that host/port in the portal and leave TLS/username/password
blank. The host's LAN IP changes per machine/network, and anonymous
access is fine for a closed bench network only — don't reuse this
config for a real install.

Watch traffic with `mosquitto_sub -h <that IP> -t 'kilowatts/#' -v`.

## Dependencies

Requires ESP-IDF's `esp-mqtt` client (`mqtt_client.h`) and `cJSON`
(`cJSON.h`), declared as Component Manager dependencies in
`src/idf_component.yml` (`espressif/mqtt`, `espressif/cjson`) rather than
vendored, and is therefore ESP32-target-only — like `WiFiManager`,
`EspNowCommunication` and `ChipInfo`, it has no host build split and no
host-native test. Broker credentials never live in this module: an
installer-provisioned broker persists via `MqttCredentialsStore` (NVS,
same pattern as `WiFiCredentialsStore` — see that module's own README and
`WiFiProvisioningPortal`, which is what actually lets an installer submit
one), and the compiled-in cloud-broker fallback lives in
`include/KilowattsSecrets.h` (gitignored).

## Boundary

Does not compute battery/power values, does not run Best-First Search,
does not actuate relays, and does not own Wi-Fi connectivity
(`WiFiManager` does) — it only carries JSON payloads to and from the
broker and dispatches parsed commands to the caller.

---

## SystemStateJson


Formats the `kilowatts/v1/state/system` MQTT payload — the battery,
power-limit and connectivity summary the mobile application needs, so it
never has to recompute any Chapter 4 mathematics itself.

## Responsibility

```cpp
SystemStateInputs inputs{};
inputs.batteryVoltageVolts = battery bus V;
inputs.stateOfChargePercent = batteryStateOfCharge.getStateOfChargePercent();
inputs.estimatedTotalLoadPowerWatts = relay/rating estimate;
inputs.availablePowerWatts = safePowerLimit.getAvailablePowerWatts();
inputs.remainingPowerWatts = search.getRemainingPowerWatts();
inputs.wifiConnected = wifiManager.isConnected();
inputs.batteryMeasurementSourceText = developmentSession.isActive() ? "SIMULATED" : "HARDWARE";
// ... every other already-computed field ...

const std::string payload = SystemStateJson::build(inputs, CentralNodeConfig::MQTT_SCHEMA_VERSION);
mqttManager.publish(MqttManager::TOPIC_STATE_SYSTEM, payload, /* qos */ 1, /* retain */ true);
```

`SystemStateJson` has zero business logic — every field in
`SystemStateInputs` is already computed by the module that owns it
(`BatteryStateOfCharge`, `SafePowerLimitCalculator`, `AvailablePowerManager`,
`BestFirstSearch`, `WiFiManager`, `MqttManager`, `CurrentTimeProvider`).
It only formats them into the fixed JSON schema, hand-formatted via
`snprintf`/string concatenation rather than a JSON library, since the
schema is small and fixed and this keeps the class free of any external
dependency. String fields (`faultSummaryText`) are properly JSON-escaped.

`estimatedTotalLoadPowerWatts` is intentionally an estimate: Smart-node
loads are planned from installer-entered voltage/current ratings and relay
confirmation, not a collection of per-load INA219 readings. The battery
fields are the only live current-sensor telemetry in this design.

## Host build

Plain C++, no ESP-IDF dependency at all — always compiled and fully
host-testable (see `test/SystemStateJson/`).

## Boundary

Does not calculate anything, does not publish anything (`MqttManager`
does that), and does not decide the tree topology (`TopologyTree`).

