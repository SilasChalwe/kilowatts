# FAQ

**Why does simulation mode use the exact same code path as real hardware?**
Because the only actual difference between a real battery and a simulated one is where the voltage/current/SoC numbers come from — everything downstream (power budget math, Best-First selection, MQTT/console reporting) is identical either way (`TECHNICAL_REFERENCE.md`). Keeping one path instead of two means simulation is a genuine sizing tool (`INSTALLATION_GUIDE.md`), not a separate "demo mode" that could behave differently from what actually ships.

**Why doesn't the firmware ship with default battery/sensor values?**
Every battery, INA219 shunt resistor, and site's power policy is different. A default would either be wrong for most installations or would silently mask a missing real configuration. See `LIMITATIONS.md` and `USER_MANUAL.md` §4.

**Why is there no encryption beyond standard TLS (no certificate pinning, no client certs)?**
Scope decision: implementing and validating mTLS was judged not worth the remaining project time. Standard TLS via the ESP-IDF CA bundle is still in place — this is a "not yet hardened further" gap, not an unencrypted connection. See `FUTURE_WORK.md`.

**Why is each node capped at 16 Loads?**
It's the current `NodeLoadHardwareStore::MAX_CONFIGURED_LOADS` value, chosen to match what a single ESP-NOW report can carry in 4 pages of `MAX_LOADS_PER_NODE_PACKET` (4 Loads/page — the largest count that reliably fits under ESP-NOW's 250-byte payload limit once the wire struct's fixed fields are subtracted). It is a real firmware constant, not a hardware limit — see `lib/NodeManager/NodeReportPackets.h`.

**What happens if a Smart Node never acknowledges a command?**
No second `acks` message is ever published for that `commandId` — there's no timeout/failure event on the MQTT side. A client-side timeout is the app's own responsibility. See `lib/MqttManager/README.md`'s "Two-phase acknowledgement" section.

**Why does a Load's `mode` field change on its own for AUTO Loads?**
`AUTO_ON`/`AUTO_OFF` records the *result* of the last Best-First cycle, not a command — it's re-evaluated from scratch every cycle. It is not evidence of an external change; it's the planner doing its job. `FIXED_ON`/`FIXED_OFF` never change on their own.

**Why doesn't an `acks` message prove a real appliance switched on?**
The firmware only knows it commanded a GPIO/relay pin — it has no feedback mechanism for what (if anything) is physically wired downstream. See `LIMITATIONS.md` "No physical downstream confirmation" and `USER_MANUAL.md` §6.

**Can I trust a load's priority alone to predict whether it'll be selected?**
Not in isolation — Best-First compares total priority across entire *combinations* of Loads, not one Load against another. Two Loads tied at the same priority don't necessarily mean one wins over the other; see `QA_REPORT_LOAD_SELECTION_SCENARIOS.md` Scenario 3 for a measured example, and `TECHNICAL_REFERENCE.md` for the exact ranking rules.

**Where do I find the exact JSON shape for a specific MQTT topic?**
`lib/MqttManager/README.md` — every published and subscribed topic, field by field, with valid ranges and examples.

**Is this project considered finished?**
The firmware is complete against its stated goals and every fix made this session is live-verified on real hardware (`test-evidence/`, `QA_REPORT_LOAD_SELECTION_SCENARIOS.md`). What remains is listed plainly in `LIMITATIONS.md` (multi-node network validation, a real INA219, mTLS) and `FUTURE_WORK.md` (ideas for what comes next) — neither is a hidden gap, both are explicit, intentional scope boundaries.
