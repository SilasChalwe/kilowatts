# Limitations

Stated plainly, as fact rather than excuse — what was and wasn't tested, and why.

## Hardware availability

The project originally had 3 ESP32 boards and 2 IoT sensor modules available for testing a real multi-node network (one Central plus multiple Smart Nodes, each with its own physical sensor). During the project, 2 of the 3 ESP32 boards and 1 of the 2 sensor modules were physically damaged and became unusable, leaving one working ESP32 board.

**Consequence**: every live-hardware test in `test-evidence/` and `QA_REPORT_LOAD_SELECTION_SCENARIOS.md` was run on that single board, reflashed between the Central and Smart Node firmware roles as needed, rather than on a real multi-node network running simultaneously. This is enough to verify each role's own logic in isolation (the Central console, the Smart Node console, MQTT, Best-First selection, battery simulation) — it does not verify multi-node behavior under real conditions: real ESP-NOW mesh routing/hop-count across more than one physical hop, real concurrent reporting from several Smart Nodes at once, or real network-partition/recovery behavior with more than one device.

One testing artifact of this setup, not a firmware defect: Central and a Smart Node share the same NVS key names (`kw_node_loads`/`loads`) for their own local load configuration. On separate physical devices (a real deployment) this causes no collision — each device has independent flash. It only surfaced because this session's testing reused one physical chip for both roles, so Central's own loads were visible to the Smart firmware's duplicate-pin check when both were exercised on the same board back to back.

## No real INA219 sensor tested

No physical INA219 battery monitor chip was available to test against. Every "hardware mode" (`sensor ina219`) check this session correctly reported the sensor as not responding — the I2C probe path itself (`PowerManager::initializeSensor()`/`isHardwareSensorPresent()`) was exercised and behaves correctly on failure, but a real, successful hardware reading was never observed. Simulation mode was used throughout instead, which the firmware treats as functionally identical downstream of the measurement source (see `TECHNICAL_REFERENCE.md`) — but that equivalence itself has not been cross-checked against a real sensor's actual noise/accuracy characteristics.

## No client-certificate / pinned TLS

The MQTT connection uses TLS via the standard ESP-IDF CA certificate bundle (`esp_crt_bundle_attach`) — standard server certificate validation, same as a browser. There is no certificate pinning and no client certificate (mTLS). This was a deliberate scope decision, not an oversight: implementing and testing mTLS was judged not worth the time against the project's schedule, and is listed as a recommended addition in `FUTURE_WORK.md` rather than attempted here.

## No long-run measurement

`QA_REPORT_LOAD_SELECTION_SCENARIOS.md`'s scenarios are point-in-time measurements (a specific SoC/voltage/current snapshot, immediately re-checked). There is no multi-day or multi-week measurement of actual watt-hour savings, battery cycle behavior, or runtime accuracy under real load variation — that would require the real multi-node hardware and real appliances this project did not have available (see above).

## No physical downstream confirmation

The firmware writes a GPIO/relay command and reports success once that write completes at the firmware level (per `USER_MANUAL.md` §"Command acknowledgement meaning"). It has no way to confirm what, if anything, is actually connected downstream of that pin or whether it physically switched. This is an architectural boundary, not a bug — adding real feedback (a current-sensing or relay-state-confirmation mechanism) is future work, see `FUTURE_WORK.md`.

## Uncommissioned Smart Node persisted-state edge case

The standalone Smart Node console was verified for add/show/set/list/remove in
`test-evidence/INSTALLATION_TEST_REPORT.md`. One known edge case remains: if an
uncommissioned Smart Node contains stale persisted Load records, the store and
the live in-memory Node can disagree after boot because persisted Loads are
currently re-applied only after commissioning. This was observed while one
board was reused between roles and was not resolved in the completed scope.
Clear stale configuration before commissioning a reused board. A boot-time
state-reconciliation improvement is listed in `FUTURE_WORK.md`.
