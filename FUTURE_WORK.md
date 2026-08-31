# Future Work

Forward-looking items raised during this project, kept separate from `LIMITATIONS.md` (what wasn't tested) because these are genuine design changes, not just missing verification.

## Per-appliance power sensing, replacing the configured nameplate rating

Today, `powerRatingWatts` is a number entered once at install time (`USER_MANUAL.md` §2) — the planner trusts it, with no way to know the Load's actual real-time draw. The natural next step: give each relay channel its own current/power sensing (an INA-class chip or a shunt per channel, one per GPIO a Load is wired to), so a Load reports what it's *actually* drawing instead of what it was configured to draw. This would let Best-First plan against real numbers instead of installer estimates, and would open the door to detecting a Load that's drawing nothing (nothing plugged in, appliance faulty) versus one drawing exactly its rated power — directly closing the "no physical downstream confirmation" gap in `LIMITATIONS.md`. This is a real hardware + firmware redesign (each Load's model would need a live `currentDrawWatts` field alongside the configured `powerRatingWatts`, and the report packet's wire format would need to carry it), not a small patch — worth scoping as its own project phase.

## mTLS / certificate pinning for MQTT

Current TLS uses the standard ESP-IDF CA bundle (server validation only). Adding a pinned certificate or client certificate would harden the MQTT connection against a broker impersonation attack. Deliberately deferred this project (`LIMITATIONS.md`) — worth doing before a production rollout at scale.

## Real multi-node network validation

Everything in `test-evidence/` and `QA_REPORT_LOAD_SELECTION_SCENARIOS.md` ran on one physical board (see `LIMITATIONS.md` for why). Once replacement hardware is available: validate real ESP-NOW multi-hop routing, concurrent reporting from several Smart Nodes, and node-offline/recovery behavior (`alerts`' `NODE_OFFLINE` type) under real network conditions rather than a single board's own view of itself.

## Long-run measured savings

Point-in-time scenario measurements exist (`QA_REPORT_LOAD_SELECTION_SCENARIOS.md`); a genuine multi-day/multi-week deployment measuring real watt-hour savings and runtime accuracy against a real battery under real load variation does not. Worth pursuing once a stable multi-node install is running at a real site.

## Generalizing beyond per-appliance configuration

Today an installer configures each appliance individually (`load add`). A more general model — e.g. a Node that self-reports its available relay/IO capabilities and lets Central (or an app) assign a Load to any of them dynamically, rather than an installer hand-typing `pin=`/`type=`/`active_high=` per appliance — would make onboarding faster and less error-prone at scale. `state/nodes`'s existing `availableRelayPins` field (`lib/MqttManager/README.md`) is a first step toward this; a UI that reads it and drives `CONFIGURE_LOAD` without the installer needing to know raw GPIO numbers would be the natural next step.

## Real-time system considerations

This is a real-time-adjacent system (relay commands need to happen promptly relative to power-budget changes), but nothing in the current architecture enforces hard real-time guarantees — the optimizer runs on a configurable interval (default 5 minutes, `SET_OPTIMIZER_INTERVAL`) plus event-triggered wakeups, not a hard deadline scheduler. Worth a deliberate pass if this is ever deployed somewhere a delayed relay command has safety consequences, rather than just cost/comfort ones.

## Fix console schedule display to show the full window

`load show <pin>` prints only `AUTO schedule : 15:00` instead of the
complete `15:00-15:16` window. Code inspection during
`test-evidence/FOUR_LED_HARDWARE_TEST_2026-08-31.md` ("Schedule display
finding") confirmed the schedule parser and evaluator already store and use
both the start and end time correctly — this is a display-formatter bug only
(it prints `startHour`/`startMinute` and never reaches the end fields), not a
scheduling-logic bug. Small, scoped fix: update the status formatter to print
the stored end time too.

## Surface which constraint is binding, not just the resulting numbers

Testing a 100/200/300/400 Ah capacity comparison at a fixed runtime/SoC
(`test-evidence/FOUR_LED_HARDWARE_TEST_2026-08-31.md`, "Battery-capacity
comparison") showed `P_auto_available` rising with capacity until it hits the
fixed `P_budget − P_reserve − P_fixed` ceiling, after which more battery
capacity buys nothing further — correct behavior per the documented formula
(`INSTALLATION_GUIDE.md` §4), but the dashboard only shows the resulting
watt values, not *why* a bigger battery didn't unlock another load. A
dashboard/MQTT `state` addition that names the currently-binding constraint
(`P_budget ceiling` vs. `runtime-sustainable energy` vs. `minimum SoC`) would
save an installer from concluding their bigger battery "isn't working."

## Auto-simulate current from selected loads

`sensor values` requires manually supplying a current that approximately
matches whichever loads you expect the planner to select — the optimizer
does not derive simulated current from relay state
(`test-evidence/FOUR_LED_HARDWARE_TEST_2026-08-31.md`, "Staged battery-
discharge simulation"). An optional simulation mode that computes simulated
current as the sum of currently-ON loads' `powerRatingWatts` divided by
voltage would remove a manual, error-prone step from every future
simulation-based test and let scenario testing scale to more loads without
hand-computed current values.

## Consider a guarded remote path for battery installation values

Battery physical capacity, nominal voltage, starting SoC, and INA219 shunt
setup are deliberately console-only, not exposed as frontend MQTT commands
(`lib/MqttManager/README.md`, "Installation boundary") — confirmed while
scoping the MQTT test in
`test-evidence/FOUR_LED_HARDWARE_TEST_2026-08-31.md`. This is a reasonable
default (it stops a compromised or buggy frontend from silently rewriting
what the battery physically is), but it also means a future mobile app has
no way to help an installer reconfigure these after something like a battery
swap without physical serial access. Worth a deliberate decision — keep this
console-only permanently as a safety boundary, or add a narrowly-scoped,
explicitly-confirmed MQTT command for it — rather than leaving it as an
accidental gap.

## Smart Node boot-time state reconciliation

Resolve the known case where an uncommissioned Smart Node has stale persisted
Load records but its live Node object starts empty. Boot should either reapply
valid persisted records consistently, or erase them as part of an explicit
factory/uncommissioned-state policy. Add a reboot test covering both a clean
uncommissioned board and a reused board with persisted Loads. See
`LIMITATIONS.md`.
