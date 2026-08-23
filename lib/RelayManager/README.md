# RelayManager

`RelayManager` is the GPIO-output layer used by the current Kilowatts prototype. The existing class/file names keep the word **Relay** because the prototype is commonly wired through relay modules, but the software contract is simpler: Kilowatts configures a GPIO and writes the electrical level required for logical ON/OFF.

## What the firmware knows

For a registered control channel the firmware knows:

- the ESP32 GPIO number (`relayPin` in the current API; this is the Load control pin),
- whether logical ON is active-high or active-low,
- whether ESP-IDF successfully configured the GPIO,
- whether `gpio_set_level()` returned success for a requested write.

It does **not** know what is connected after that GPIO and has no downstream-device feedback mechanism. A successful GPIO write is not evidence that an external relay, transistor, MOSFET, contactor, appliance, or other device actually changed state.

## `RelayController`

`RelayController` registers the GPIO control channels owned by one Node and performs local GPIO writes.

```cpp
RelayController relays;
relays.addRelay({16U, false, false}); // pin 16, active-low, initial logical OFF
const bool ok = relays.setRelayState(16U, true);
```

`setRelayState()` translates the requested logical state through the configured polarity and calls `gpio_set_level()` on ESP32 builds. Its return value means only that the GPIO write API succeeded.

`isHardwareApplied()` reports whether the GPIO was successfully configured by the ESP-IDF driver. It is not downstream-device feedback.

The controller intentionally has no physical-state confirmation model and no per-Load target/applied/confirmed state storage.

## `RelayCommandDispatcher`

Central uses `RelayCommandDispatcher` for two jobs:

1. order requested GPIO commands with OFF requests before ON requests;
2. track remote command IDs until a Smart Node acknowledges processing or the caller times out.

A Smart Node acknowledgement reports whether its requested GPIO write was processed successfully. It does not confirm anything connected after the GPIO.

## Central and Smart Node control

Central-local Load:

```text
planner decision -> RelayController -> local gpio_set_level()
```

Smart Node Load:

```text
planner decision -> ESP-NOW command -> Smart Node RelayController -> gpio_set_level()
                                              |
                                              +-> ACK: command/GPIO write result only
```

The same `Load` model is used in both cases.
