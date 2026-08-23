# NodeManager

`NodeManager` owns Node identity, commissioning, topology/registry data, per-Node Load hardware configuration, and the ESP-NOW packet structures shared by Central and Smart Nodes.

## Nodes

Central is itself a `Node` and may own local Loads. The system therefore works with only Central and zero Smart Nodes.

When Smart Nodes are commissioned, Central combines:

- Central-local Loads,
- commissioned Smart Node Loads,

into one planning view.

## Load identity

A Load is identified by:

```text
Node MAC + relayPin
```

`relayPin` is the current API/protocol name for the GPIO control pin on the owning Node.

## `NodeLoadHardwareStore`

Both Central and Smart Nodes use the same `NodeLoadHardwareStore` and the same `Load` model. A stored hardware configuration contains:

- name,
- GPIO/control pin (`relayPin`),
- output polarity (`relayActiveHigh`),
- `powerRatingWatts`,
- AC/DC power type,
- mode,
- priority,
- optional AUTO schedule.

The store configures the GPIO through `RelayController`, creates/removes the corresponding `Load` in the owning `Node`, and persists the configuration.

The store does not verify the behaviour of any downstream device connected to the GPIO.

## Smart Node reports

`NodeReportPacket` carries the current Load planning/configuration data needed by Central. It deliberately does not carry:

- startup watts,
- per-Load nominal voltage/current,
- target state,
- confirmed state,
- physical relay state.

Central may apply its persisted planning overrides (priority/mode/schedule) to the registry copy after receiving a Smart Node report.

## GPIO command acknowledgement

`RelayCommandPacket` carries:

- control pin,
- requested logical ON/OFF,
- command ID.

`RelayCommandAcknowledgementPacket` carries:

- control pin,
- command ID,
- requested state,
- success flag,
- failure reason.

A successful acknowledgement means the Smart Node accepted the command and its GPIO write call succeeded. It does not verify anything connected after the GPIO.

## Commissioning and topology

Central maintains commissioning state and topology/route information for Smart Nodes. Central cannot decommission itself. Smart Node configuration commands are routed over ESP-NOW when a route exists.

## Shared user command model

`SystemCommandModel.h` defines the canonical request/result structures used by both serial Console and MQTT. The transports may parse different encodings, but domain operations use the same request models and handlers.
