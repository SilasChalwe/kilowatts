# EspNowCommunication

Dynamic ESP-NOW communication for the Kilowatts tree network. One
`EspNowCommunication` object represents the ESP-NOW capability of THIS
ESP32 — Central or Smart, the class itself does not care which.

## Responsibility

THIS ESP32 obtains its own Wi-Fi station MAC address from `ChipInfo`
rather than reading it separately. A Smart Node does not need a manually
configured upstream Node MAC address: it broadcasts a discovery request,
receives responses from nearby Nodes that already have a route to
Central, and dynamically selects its Upstream Node.

```cpp
EspNowCommunication communication(1U);          // Wi-Fi channel
communication.setLocalNodeName("Sitting Room");
communication.initialize();                      // NVS, Wi-Fi STA, ESP-NOW, callbacks

// Central:
communication.setAsCentralNode();

// Smart Node:
while (!communication.discoverUpstreamNode(2000U)) {
    // retry until a route to Central is found
}
```

## Discovery

`discoverUpstreamNode()`:

1. Broadcasts a `DISCOVERY_REQUEST`.
2. Nodes that already have a route to Central respond with a
   `DISCOVERY_RESPONSE` carrying their own hop count and Central's MAC
   address.
3. Nodes that do not already have a route to Central are ignored — this
   prevents a Node from selecting one of its own descendants as its
   upstream.
4. The candidate with the lowest hop count to Central wins; ties are
   broken by the stronger RSSI.
5. The winning candidate becomes this Node's Upstream Node
   (`hasUpstreamNode()`, `getUpstreamNodeMacAddress()`,
   `getHopCountToCentral()`).

## Messages

Every message carries a `MessageHeader` (protocol version, `MessageType`,
payload length, message ID, **origin** MAC address, **final destination**
MAC address, hops remaining) followed by up to `MAX_PAYLOAD_SIZE` bytes of
payload. The immediate sender of one radio hop is *not* stored in the
header — ESP-NOW supplies that separately on receive
(`ReceivedMessage::senderMacAddress`), which is how a multi-hop chain
distinguishes "who physically sent me this packet" from "who originally
created it" and "who it's ultimately for":

```cpp
enum class MessageType : std::uint8_t {
    NODE_REPORT, LOAD_REPORT, RELAY_COMMAND, ACKNOWLEDGEMENT,
    ERROR_MESSAGE, HANDSHAKE, DISCOVERY_REQUEST, DISCOVERY_RESPONSE,
    // Commissioning lifecycle (see CommissioningPackets.h in lib/NodeManager), appended
    // without renumbering the values above:
    IDENTITY_REPORT, COMMISSION_COMMAND, COMMISSION_ACK,
    DECOMMISSION_COMMAND, DECOMMISSION_ACK
};
```

Sending:

```cpp
communication.sendTo(nextHopMac, destinationMac, MessageType::NODE_REPORT, packet);   // typed helper
communication.sendToCentral(MessageType::NODE_REPORT, packet);                        // toward the selected Upstream Node
communication.forwardMessageTo(nextHopMac, receivedMessage.message);                  // relay unchanged (origin/destination/ID kept, hopsRemaining-1)
```

The typed `sendTo`/`sendToCentral` overloads `static_assert` that the
payload is trivially copyable — a live C++ object containing
`std::vector`/`std::string` (a `Node` or a `Load`) must first be converted
into a transmission packet structure (see `NodeReportPackets`)
before it can be sent.

Receiving:

```cpp
EspNowCommunication::ReceivedMessage received{};
if (communication.receive(received, 1000U)) {
    // received.senderMacAddress            — who sent this radio hop
    // received.message.header.originMacAddress       — who created it
    // received.message.header.destinationMacAddress  — final recipient
    // received.signalStrengthDbm           — RSSI of this hop
}
```

`receive()` only ever returns application traffic — discovery request/
response and connection-handshake traffic are handled internally by the
background service task and never surfacing to the caller.

## Downstream connections

A directly attached downstream Node is normally registered automatically
when its connection handshake is physically received. It can also be
registered manually for a simulated/known relationship (used by the
current behaviour tests) via `registerDirectDownstreamNode()`.
`printConnectionInfo()` logs THIS Node and its directly attached
downstream Nodes only — a Node that is not directly attached (for example
a grandchild) is not shown in that table even though it may still be
reachable through the reconstructed multi-hop tree.

## Boundaries

`EspNowCommunication` is a transport. It does not know about `Node`/`Load`
domain objects, does not decide Load selection or Available Power, and
does not run Best-First Search or MQTT — it moves bytes between MAC
addresses and tracks the tree topology needed to route them
(`NodeReportPackets` defines what those bytes mean;
`CentralNodeRegistry` turns them into domain objects on the Central side).

ESP-NOW callbacks are static, so this class assumes exactly one
communication manager instance per ESP32.
