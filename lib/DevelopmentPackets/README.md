# DevelopmentPackets

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
