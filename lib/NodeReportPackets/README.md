# NodeReportPackets

The shared wire-format structures sent between Kilowatts ESP32 firmware
over ESP-NOW — header-only, no `.cpp`.

## Responsibility

These structures are the byte layout carried inside an
`EspNowCommunication::Message` payload. They are a **transport**
representation only — a `NodeReportPacket` is how one Node's report
travels over the radio, not the `Node`/`Load` domain object used for
planning (see `Node` and `Load` for that; `CentralNodeRegistry` is what
converts one into the other).

This module was previously named `PowerManagementMessages`, which was a
misleading name: it does not manage power. Every structure here is an
actual ESP-NOW wire-format payload, not a general DTO and not a
power-management domain object — hence `Packet` in every type name.

```cpp
struct LoadReportPacket {
    char name[16];
    std::uint8_t relayPin;
    std::uint8_t mode;                     // raw kilowatts::LoadMode::Value byte (configured)
    std::uint16_t priority;
    float runningWatts;
    float startupWatts;
    float branchMaximumCurrentAmps;        // this relay pin's Branch configuration, I_branch,max
    float measuredVoltageVolts;            // latest filtered (EMA) INA219 measurement
    float measuredCurrentAmps;
    float measuredPowerWatts;
    std::uint8_t confirmedRelayState;      // last GPIO read-back (0=OFF, 1=ON) — physical truth
    std::uint8_t confirmedRelayStateValid; // Load::isConfirmedRelayStateValid(): 1=trustworthy, 0=unknown
    std::uint8_t scheduleEnabled;
    std::uint8_t scheduleHour;
    std::uint8_t scheduleMinute;
    std::uint8_t availability;             // raw LoadAvailability byte
};

struct NodeReportPacket {
    char nodeName[20];
    EspNowCommunication::MacAddress nodeMacAddress;
    EspNowCommunication::MacAddress upstreamNodeMacAddress;   // this Node's Next Hop to Central
    std::uint16_t hopCountToCentral;
    std::uint8_t numberOfLoads;
    std::uint16_t reportSequenceId;        // shared by every page of one report cycle
    std::uint8_t pageIndex;                // 0-based
    std::uint8_t totalPages;               // page count for this reportSequenceId
    std::array<LoadReportPacket, MAX_LOADS_PER_NODE_PACKET> loads;
};

struct NodeReportAcknowledgementPacket {
    EspNowCommunication::MacAddress nodeMacAddress;
    std::uint32_t receivedMessageId;
};

struct RelayCommandPacket {
    std::uint8_t relayPin;
    std::uint8_t desiredState;             // raw RelayCommandState byte
    std::uint32_t commandId;
};

struct RelayCommandAcknowledgementPacket {
    std::uint8_t relayPin;
    std::uint32_t commandId;
    std::uint8_t requestedState;           // raw RelayCommandState byte
    std::uint8_t confirmedState;           // raw RelayCommandState byte, valid only when success=1
    std::uint8_t success;
    std::uint8_t failureReason;            // raw RelayCommandFailureReason byte
};
```

`MAX_LOADS_PER_NODE_PACKET = 3` is a **packet-page capacity** (one report
*page* must fit inside one ESP-NOW message; ESP-NOW v1.0, which the
Central Node's classic ESP32 is limited to, caps one message at 250
bytes), not a limit on how many Loads a real Node may physically control.
It was sized to exactly match the current physical Smart Node's three
demonstration Loads (see `include/SmartNodeConfig.h`), so today's report
is always exactly one page (`pageIndex = 0`, `totalPages = 1`). A future
Node with more Loads than fit in one page sends several
`NodeReportPacket` messages sharing the same `reportSequenceId`, with
`pageIndex` counting up to `totalPages` — the receiver (`CentralNodeRegistry`)
reassembles them rather than the domain being capped at one page's worth
of Loads.

The destination Node for a `RelayCommandPacket` /
`RelayCommandAcknowledgementPacket` is already carried by the surrounding
`EspNowCommunication::Message` header
(`destinationMacAddress`/`originMacAddress`) — these packets only ever
identify *which relay pin on that Node*, never a Node MAC address again.
`BestFirstSearch::BranchId` (and a `LoadReportPacket`'s Branch identity)
is always reconstructed as `{NodeReportPacket::nodeMacAddress,
LoadReportPacket::relayPin}` for the same reason: a Load's owning Node is
already known from the enclosing report, so it is never repeated per Load.

Central and Smart firmware previously each declared their own copy of
these structures by hand. Because both sides must agree on the exact same
byte layout to interpret a received payload correctly, the two copies were
a system contract that could silently drift apart if only one file was
edited — they are declared once here instead and included by both
`src/central/main.cpp` and `src/smart/main.cpp`.

Several `static_assert`s enforce the contract at compile time: every
structure here must be trivially copyable (safe to send/receive as raw
bytes over ESP-NOW), and `NodeReportPacket`/`RelayCommandPacket`/
`RelayCommandAcknowledgementPacket` must each fit inside
`EspNowCommunication::MAX_PAYLOAD_SIZE`.

## Boundaries

This header defines byte layout only. It does not perform the actual
send/receive (`EspNowCommunication`), does not decide message routing or
topology, does not read INA219 hardware or actuate relays (`INA219Monitor`,
`RelayController`), and does not decide *when* a report is sent or *what*
relay state to command — it only describes what those decisions travel as
over the radio.
