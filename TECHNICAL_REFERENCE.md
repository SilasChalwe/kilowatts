# Technical Reference — Formulas and Algorithms

This document explains the two pieces of math the firmware runs every optimization cycle: the runtime/power budget (how much power is available to spend), and Best-First Search (which AUTO Loads get that power). Both are implemented once, in `lib/BatteryManager/PowerManager.cpp` and `lib/BestFirstSearch/BestFirstSearch.cpp`, and used identically regardless of whether the battery reading is real (INA219) or simulated — see `USER_MANUAL.md` §2.

## 1. Power budget (P_available)

```text
fullBatteryEnergyWh      = batteryCapacityAh × nominalVoltageVolts
usableSoCFraction        = max(0, currentSoCPercent − reserveSoCPercent) / 100
usableEnergyWh           = fullBatteryEnergyWh × usableSoCFraction

sustainableTotalPowerW   = usableEnergyWh / remainingRequiredRuntimeHours   (only if a runtime target is configured)
P_battery_max            = batteryVoltage × maximumBatteryDischargeCurrentAmps
P_main_max                = batteryVoltage × maximumMainCurrentAmps

P_committed               = sum of every FIXED_ON Load's powerRatingWatts
P_remaining                = min(P_battery_max, P_main_max) − P_committed

P_available (final)       = min(P_remaining, sustainableTotalPowerW − P_committed)   (the runtime target, when configured, is the stricter of the two limits)
```

If `currentSoCPercent ≤ reserveSoCPercent`, `P_available` is forced to `0` regardless of every other term — battery protection always wins. If no runtime target is configured (`requiredRuntimeHours = 0`), only the immediate electrical limit (`P_remaining`) applies.

`P_available` is what gets handed to Best-First Search as the budget for AUTO Loads. FIXED_ON Loads are never candidates for selection — their power is subtracted before AUTO planning starts, and FIXED_OFF Loads never draw power at all.

## 2. Best-First Search — which AUTO Loads get selected

Source: `lib/BestFirstSearch/BestFirstSearch.cpp`. This is a genuine best-first graph search over *combinations* of AUTO Loads (candidate subsets), not a simple sort-by-priority.

**Per-Load effective priority:**

```text
effectivePriority(load) = load.priority + (5 if load has an AutoSchedule window active right now, else 0)
```

The `+5` boost (`ACTIVE_SCHEDULE_PRIORITY_BOOST`) only applies while the load's configured `HH:MM-HH:MM` window contains the current time — outside the window, effective priority equals the configured priority. Verified live: `QA_REPORT_LOAD_SELECTION_SCENARIOS.md` Scenario 2.

**Per-combination scoring**, for any candidate subset of AUTO Loads:

```text
totalEffectivePriority(combo) = sum of effectivePriority(load) for every load in combo
g(combo) = combo.size()                                    (search depth — number of loads)
h(combo) = average(load.powerRatingWatts for load in combo)  (heuristic cost)
f(combo) = g + h
```

**Search**: starts from the empty combination and repeatedly expands the best-ranked open state by adding one more AUTO Load, rejecting any child whose total wattage exceeds `P_available` (infeasible combinations are never explored further — verified live: Scenario 1, a 10 W load never gets selected against a ~6 W budget no matter its priority).

**Ranking** (both for which state to expand next, and for choosing the final answer), in order:
1. Highest `totalEffectivePriority` wins.
2. Then highest *configured* priority sum (without the schedule boost) as a secondary tiebreak.
3. Then lowest `f = g + h`.
4. Then more loads selected (larger combination) wins.
5. Finally, lowest total wattage wins.

**Important nuance, confirmed live** (Scenario 3): rule 1 operates on the *combination's total*, not on any single load. Two individually-tied loads do not automatically tie the outcome — a combination containing neither of them can still win if its own total effective priority is higher. The g/h/wattage tiebreaks (rules 3–5) only come into play when two *combinations* have equal total priority, which is less common than it might sound given rule 1 dominates almost every real comparison.

**Mode semantics**: `FIXED_ON`/`FIXED_OFF` are never passed to Best-First — they're forced every cycle. `AUTO_ON`/`AUTO_OFF` are the *result* of the last cycle's selection, not a command — every cycle re-evaluates from scratch, so an `AUTO_OFF` load is exactly as eligible as an `AUTO_ON` one on the next run.

**Relay dispatch**: selected loads are switched ON with a staggered delay (3 s before the first, 2 s between each subsequent one) rather than all at once — see `CentralNodeConfig::RELAY_ON_FIRST_DELAY_MILLISECONDS`/`RELAY_ON_BETWEEN_DELAY_MILLISECONDS`.

## 3. Where these numbers surface

- Console: `battery status`, `dashboard`, `optimize run`.
- MQTT: `state/system`'s `battery`/`powerFlow` objects (see `lib/MqttManager/README.md`).
- Per-load rejection reason: `state/loads[].bestFirstRejectionReason` — `NONE`, `LOW_BATTERY`, `POWER_BUDGET_EXCEEDED`, `BATTERY_CURRENT_LIMIT`, `MAIN_LIMIT_EXCEEDED`, `BRANCH_LIMIT_EXCEEDED`, `UNKNOWN`.
