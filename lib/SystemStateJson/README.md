# SystemStateJson

Formats the `kilowatts/v1/state/system` MQTT payload — the battery,
power-budget and connectivity summary the mobile application needs, so it
never has to recompute any Chapter 4 mathematics itself.

## Responsibility

```cpp
SystemStateInputs inputs{};
inputs.batteryVoltageVolts = battery bus V;
inputs.stateOfChargePercent = batteryStateOfCharge.getStateOfChargePercent();
inputs.availablePowerWatts = powerBudget.getAvailablePowerWatts();
inputs.remainingPowerWatts = search.getRemainingPowerWatts();
inputs.wifiConnected = wifiManager.isConnected();
inputs.sensorInputSourceText = KILOWATTS_DEVELOPMENT_MODE ? "DEVELOPMENT" : "INA219 HARDWARE";
// ... every other already-computed field ...

const std::string payload = SystemStateJson::build(inputs, CentralNodeConfig::MQTT_SCHEMA_VERSION);
mqttManager.publish(MqttManager::TOPIC_STATE_SYSTEM, payload, /* qos */ 1, /* retain */ true);
```

`SystemStateJson` has zero business logic — every field in
`SystemStateInputs` is already computed by the module that owns it
(`BatteryStateOfCharge`, `PowerBudgetCalculator`, `AvailablePowerManager`,
`BestFirstSearch`, `WiFiManager`, `MqttManager`, `CurrentTimeProvider`).
It only formats them into the fixed JSON schema, hand-formatted via
`snprintf`/string concatenation rather than a JSON library, since the
schema is small and fixed and this keeps the class free of any external
dependency. String fields (`faultSummaryText`) are properly JSON-escaped.

## Host build

Plain C++, no ESP-IDF dependency at all — always compiled and fully
host-testable (see `test/SystemStateJson/`).

## Boundary

Does not calculate anything, does not publish anything (`MqttManager`
does that), and does not decide the tree topology (`TopologyTree`).
