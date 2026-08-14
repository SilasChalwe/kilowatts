# DevelopmentSession

The explicit, runtime-only Development Session a Node's Operating
Environment can enter — replacing the old `KILOWATTS_DEVELOPMENT_MODE`
compile-time switch, which made every boot permanently "development"
regardless of what was actually connected.

## Responsibility

```cpp
DevelopmentSession session;

session.getEnvironment();   // OperatingEnvironment::PRODUCTION — always, on every boot
session.isActive();         // false

session.start();            // -> DEVELOPMENT (only ever reached by an explicit command)
session.setSensorOverride(0x40U, /* V */ 11.0F, /* I */ 1.5F);

float v = 0.0F, i = 0.0F;
if (session.findSensorOverride(0x40U, v, i)) {
    // the sensor-acquisition call site applies this at the real
    // measurement boundary — see INA219Monitor::setDevelopmentOverride()
}

session.end();              // -> PRODUCTION, every override discarded
```

`OperatingEnvironment` (`PRODUCTION`/`DEVELOPMENT`) is deliberately a
separate state space from `LoadMode` (`Fixed`/`Auto`) — "System Environment
!= Load Mode." Every Node boots `PRODUCTION`; `DEVELOPMENT` is only ever
reached through an explicit `start()` call, never inferred from real
hardware being absent, a Node being uncommissioned, existing development
code, or a compile-time macro.

`setSensorOverride()`/`clearSensorOverride()` are rejected while the
session is not active — Development simulation requires
`START_DEVELOPMENT_SESSION` first; an override alone is never enough (see
`src/central/main.cpp`'s `commands/development` handler and
`src/smart/main.cpp`'s `DEV_SESSION_COMMAND`/`DEV_SENSOR_INPUT_COMMAND`
handling for where a real command actually calls these). `end()` discards
every armed override — nothing from one session carries into the next, or
into production.

This class only stores policy/state. It never reads INA219 hardware,
never performs ESP-NOW/MQTT, and never decides *when* a session should
start — see `INA219Monitor::setDevelopmentOverride()`/
`clearDevelopmentOverride()` for where an armed override actually changes
what a real sensor read returns, still through the exact same
calibration/EMA-filtering path a hardware reading uses.

## Persistence

None, deliberately. A Development Session and every override it armed
live only in RAM; ending the session (or simply never starting one)
leaves zero trace in NVS — matching "Development configuration overlays
do not write to production NVS."

## Host build

Entirely plain, hardware-free/network-free C++ — no `ESP_PLATFORM` split
is needed, so it is directly host-testable (see
`test/DevelopmentSession/`).

## Boundaries

Does not read or write real sensor hardware, does not perform ESP-NOW or
MQTT communication, does not persist anything, and does not decide GPIO/
Branch/Load overlay configuration (that remains a later phase's concern) —
it only tracks whether Development is currently active and which sensors
currently have an armed override.
