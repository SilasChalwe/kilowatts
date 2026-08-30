# QA Report: Load Selection and Power Budget Scenarios

## Scope

This report documents the verification status of the Kilowatts Best-First load-selection logic. Unlike the previous version of this report, every number below is measured from the live physical board (ESP32-D0WDQ6, MAC `A4:CF:12:0E:32:C0`), not modeled or invented — see `test-evidence/` for the raw captures this report is built from.

## Verified baseline

```bash
pio run -e central && pio run -e smart && pio run -e smart_esp32
bash test/run_tests.sh
```

- All three firmware targets build clean.
- 49 checks, 0 failed (host-native behavioral suite, `test/main.cpp` — exercises `lib/BestFirstSearch/BestFirstSearch.cpp` without ESP32 hardware).

## The 8-Load test configuration

All scenarios below run against the same live configuration: 2 fixed loads (one ON, one OFF) and 6 AUTO loads competing for a shared power budget.

| Pin | Name | Type | Mode | Priority | Power |
|---|---|---|---|---|---|
| 4 | Fridge | AC | FIXED_ON | — | 8.00 W |
| 26 | WaterHeater | AC | FIXED_OFF | — | 15.00 W |
| 5 | Lights | DC | AUTO | 8 | 3.00 W |
| 18 | WaterPump | DC | AUTO | 5 | 10.00 W |
| 19 | Fan | DC | AUTO | 3 | 5.00 W |
| 23 | Router | DC | AUTO | 9 | 2.00 W |
| 25 | SecurityCamera | DC | AUTO | 7 | 4.00 W |
| 27 | PhoneCharger | DC | AUTO | 2 | 1.00 W |

## Scenario 1 — Baseline selection (measured)

Battery: simulated 12.6 V / 1.5 A / 75% SoC. Available power to Best-First: **6.37–6.43 W** (varies slightly cycle-to-cycle from EMA smoothing on repeated identical inputs).

**Result:** Router + Lights + PhoneCharger selected (2 + 3 + 1 = 6.00 W), total priority 19 — the combination maximizing total priority under budget, beating the alternative of WaterPump alone (10 W, doesn't fit) or Router + SecurityCamera (6 W, priority 16, lower than 19).

## Scenario 2 — Schedule-driven priority boost (measured)

Gave SecurityCamera (priority 7, normally not selected) an active schedule window covering the current time (`load set <mac> 25 schedule=14:00-15:00`). The scheduler applies a +5 effective-priority boost to an AUTO load while its window is active — SecurityCamera's effective priority became 12.

**Before:** Router + Lights + PhoneCharger selected (as Scenario 1).
**After:** Router + SecurityCamera selected (2 + 4 = 6.00 W, effective priority 9 + 12 = 21) — Lights and PhoneCharger displaced.

This is a genuine, observed behavior change driven purely by wall-clock time entering the configured window, not a predicted one.

## Scenario 3 — Priority tie (measured)

Set SecurityCamera's priority to 9, exactly tying Router (also 9), with no schedule active.

**Result:** Router + Lights + PhoneCharger still selected (priority sum 19) — **not** Router + SecurityCamera (priority sum 18), even though Router and SecurityCamera were individually tied. This is the more interesting and accurate finding: Best-First does not resolve ties between two loads in isolation — it compares total **combination** priority across every feasible subset, and a 3-load combination with a lower top individual priority beat a 2-load combination containing both tied loads, because the combination's total was higher. The g/h/wattage tie-break rule in `BestFirstSearch.cpp` only matters when two *combinations* have equal total priority, which did not occur in this run.

## Scenario 4 — Power-budget scaling (measured)

Same 8 loads, only the simulated SoC changed (75% → 90%, voltage/current held at 12.6 V / 1.5 A). This raises the runtime-sustainable power ceiling (see `TECHNICAL_REFERENCE.md`'s formula), which was the binding constraint in both readings.

| SoC | Available power | Automatic loads selected | Automatic power used |
|---|---|---|---|
| 75% | 6.37 W | 3 / 6 (Router, Lights, PhoneCharger) | 6.00 W |
| 90% | 10.78 W | 4 / 6 (Router, Lights, PhoneCharger, SecurityCamera) | 10.00 W |

At the higher SoC, every AUTO load fits except WaterPump (10 W) and Fan (5 W) — neither fits alone or combined in the remaining 0.78 W. This is a real, measured example of how the algorithm's selection scales directly with available power, rather than an invented watt-hour savings estimate.

## Distinction: verified vs. still-modeled

### Verified (measured on real hardware this session)
- The planner enforces the power budget — infeasible combinations are never selected regardless of priority (Scenario 1, WaterPump).
- Schedule windows apply their priority boost and visibly change the selected set (Scenario 2).
- Priority ties are resolved at the combination level, not the individual-load level (Scenario 3).
- Selection scales with available power in the expected direction (Scenario 4).
- Fixed loads are committed before AUTO planning and are never selected/rejected by Best-First (both Fridge and WaterHeater held their configured state through every scenario above).

### Still modeled / not measured
- Long-run watt-hour savings or runtime extension over days/weeks — this session measured instantaneous selection at specific SoC/voltage/current points, not a time-integrated savings figure.
- Real-world behavior with more than one physical node — every scenario above ran on Central's own 8 loads; see `LIMITATIONS.md` for why a real multi-node network wasn't available to test.
- Behavior against a real INA219 sensor rather than simulated input — see `LIMITATIONS.md`.

Raw console captures backing every scenario above: `test-evidence/logs/`.
