# CommissioningPackets

The commissioning-lifecycle wire-format structures sent between Kilowatts
ESP32 firmware over ESP-NOW — header-only, no `.cpp`, the same kind of
transport-only role `NodeReportPackets` already fills for measurement/relay
traffic.

## Responsibility

```cpp
struct IdentityReportPacket {
    std::uint8_t role;             // raw NodeRole byte
    std::uint8_t lifecycleState;   // raw NodeLifecycleState byte
    char firmwareVersion[12];
    char chipModel[16];
};

struct CommissionCommandPacket { std::uint32_t commandId; char friendlyName[20]; };
struct CommissionAckPacket { std::uint32_t commandId; std::uint8_t success; std::uint8_t resultingState; /* raw NodeLifecycleState byte */ };
struct DecommissionCommandPacket { std::uint32_t commandId; };
struct DecommissionAckPacket { std::uint32_t commandId; std::uint8_t success; };
```

Kept in their own file rather than added to `NodeReportPackets.h`'s
`NodeReportPacket`: that struct is already close to
`EspNowCommunication::MAX_PAYLOAD_SIZE` (it carries up to
`MAX_LOADS_PER_NODE_PACKET` `LoadReportPacket` entries) and is sent on a hot
~2s cycle, while commissioning packets are small, sent rarely, and belong to
a distinct concern — keeping them separate avoids risking the already-tested
`NodeReportPacket` layout for an unrelated change.

`IdentityReportPacket` is sent by a Node toward Central on a slow
(diagnostics-scale) cadence and immediately after any local lifecycle
change — deliberately not on `NodeReportPacket`'s hot cycle. Central's
`NodeCommissioningRegistry::recordDiscovered()` consumes it.
`CommissionCommandPacket`/`CommissionAckPacket` and
`DecommissionCommandPacket`/`DecommissionAckPacket` follow the exact
`commandId`-echo convention `RelayCommandPacket`/
`RelayCommandAcknowledgementPacket` already use, so a delayed/duplicate
acknowledgement can always be matched to the command that produced it.

As with every packet in `NodeReportPackets.h`, the owning Node is always the
surrounding `EspNowCommunication::Message` header's origin/destination MAC
address — never repeated inside these structs. `NodeRole`/
`NodeLifecycleState` come from `NodeLifecycle.h`.

`static_assert`s enforce, at compile time, that every structure here is
trivially copyable and fits inside `EspNowCommunication::MAX_PAYLOAD_SIZE`,
matching `NodeReportPackets.h`'s own pattern exactly.

## Host build

Depends on `EspNowCommunication.h`, which requires the real ESP-IDF/FreeRTOS
headers, so — like `NodeReportPackets` — this module is ESP32-target-only
with no host-native test.

## Boundaries

This header defines byte layout only. It does not perform the actual
send/receive (`EspNowCommunication`), does not decide *when* a commissioning
command should be sent (`NodeCommissioningRegistry`/`NodeIdentityStore`),
and does not validate a proposed friendly name — it only describes what
those decisions travel as over the radio.
