# DevelopmentManager

Everything to do with the runtime-only, explicitly-entered
Development Session — simulated sensor overrides and the factory
reset command — kept strictly separate from production code.
PRODUCTION is the only default on every boot, with no exception;
nothing in NodeManager, BatteryManager or any other production
library ever depends on this folder. See DevelopmentSession's own
"Boundaries" section below for the precise contract with
INA219Monitor::setDevelopmentOverride()/clearDevelopmentOverride()
(lib/BatteryManager), which is the only place an armed override
actually changes what a sensor read returns.

## DevelopmentPackets


Wire-format structures for the Development Session and Factory Reset
commands sent between Kilowatts ESP32 firmware over ESP-NOW —
header-only, no `.cpp`, the same transport-only role `CommissioningPackets`
already fills for the commissioning lifecycle.

## Responsibility

```cpp
struct DevSessionCommandPacket { std::uint32_t commandId; std::uint8_t action; /* raw DevSessionAction */ };
struct DevSensorInputCommandPacket { std::uint32_t commandId; std::uint8_t i2cAddress; std::uint8_t clearOverride; float voltageVolts; float currentAmps; };
struct DevAckPacket { std::uint32_t commandId; std::uint8_t success; std::uint8_t resultingEnvironment; };

struct FactoryResetCommandPacket { std::uint32_t commandId; std::uint8_t confirmToken; };
struct FactoryResetAckPacket { std::uint32_t commandId; std::uint8_t success; };
```

Central is always the sender of every `*_COMMAND` and the receiver of
every `*_ACK`; the owning Node is always the surrounding
`EspNowCommunication::Message` header's origin/destination MAC address,
never repeated inside these structs — the same convention every packet in
this project already follows.

`DevAckPacket::resultingEnvironment` always reflects the Node's true
`OperatingEnvironment` after processing the command, whether accepted or
rejected, mirroring `CommissionAckPacket::resultingState`'s own "always
honest" convention.

`FactoryResetCommandPacket::confirmToken` must equal
`FACTORY_RESET_CONFIRM_TOKEN` exactly (Section "Protect This Operation
From Accidental Invocation") — a mismatched token is rejected without
touching NVS. A successful reset reboots the Node immediately instead of
replying with `FactoryResetAckPacket`, since the Node is about to forget
it was ever commissioned, including any route back to Central;
`FactoryResetAckPacket` is only ever sent on rejection/failure.

## Host build

Depends on `EspNowCommunication.h`, which requires the real ESP-IDF/
FreeRTOS headers, so — like `CommissioningPackets` — this module is
ESP32-target-only with no host-native test.

## Boundaries

This header defines byte layout only. It does not perform the actual
send/receive (`EspNowCommunication`), does not decide *when* a Development
Session or factory reset should be triggered
(`DevelopmentSession`/`src/central/main.cpp`/`src/smart/main.cpp`), and
does not itself erase NVS or actuate anything — it only describes what
those decisions travel as over the radio.

---


## DevelopmentSession


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

---

