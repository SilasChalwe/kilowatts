# Best-First Search library

The complete, hardware-independent Best-First Search algorithm used by the
Kilowatts Central Node to decide which Auto candidate Loads get switched
ON within the power currently available (Sections 4.6.2-4.6.4).

## Files

- `BestFirstSearch.h` / `BestFirstSearch.cpp` — the whole algorithm: one
  `BestFirstSearch` class that scores candidates, runs the OPEN min-heap,
  and applies the full electrical constraint guard. There is no separate
  `CandidateScorer`, `ConstraintChecker`, `MinPriorityQueue` or types
  header — scoring, constraint checking and queue management are private
  methods on this one class, not separate components.

## Responsibility

`BestFirstSearch` receives Auto candidate `Load` objects (by pointer — it
does not copy or own them), a battery/electrical state for the current
planning cycle, and, for every branch a candidate belongs to, that
branch's starting committed power and maximum current. It does not know
how any of that was measured or obtained: battery State of Charge,
power-budget calculation, Fixed-Load allocation (`AvailablePowerManager`),
Load classification (`LoadFilter`) and schedule evaluation
(`LoadScheduleEvaluator`) are all handled by other modules, before their
results ever reach this class.

```cpp
BestFirstSearch search;

BestFirstSearch::Weights weights{
    /* runningPowerWeight  = */ wP,
    /* startupPowerWeight  = */ wS,
    /* batteryStressWeight = */ wB,
    /* priorityWeight      = */ wQ,
    /* scheduleWeight      = */ wT,
    /* maximumAllowedPriority = */ wMax
};
search.setSearchScoreWeights(weights);

BestFirstSearch::ElectricalPlanningState planningState{
    /* stateOfChargePercent            = */ soc,
    /* minimumStateOfChargePercent     = */ socMin,
    /* warningStateOfChargePercent     = */ socWarn,
    /* batteryBusVoltageVolts          = */ vBus,
    /* maximumBatteryPowerWatts        = */ pBatteryMax,
    /* maximumMainCurrentAmps          = */ iMainMax,
    /* totalAvailablePowerWatts        = */ availablePowerManager.getTotalAvailablePowerWatts(),
    /* powerAvailableForAutoLoadsWatts = */ availablePowerManager.getPowerAvailableForAutoLoadsWatts(),
    /* initialCommittedPowerWatts      = */ availablePowerManager.getFixedOnRunningPowerWatts()
};
search.startSearch(planningState);

// A Branch is the relay-controlled circuit feeding a Load: its identity is
// physically {owning Node MAC address, relay pin} — the same addressing
// Load::Id uses, since one relay channel feeds one Load. Maximum current is
// Branch *configuration*, supplied separately below, never part of the
// identity, so re-configuring a Branch's current limit never creates a new
// Branch.
BestFirstSearch::BranchId branchId{node.getMacAddress(), relayPin};
search.registerBranch(branchId, /* initialCommittedPowerWatts = */ 0.0F, /* maximumCurrentAmps = */ branchLimit);

for (const Load* candidate : autoCandidateLoads) {
    search.addLoad(*candidate, branchId, scheduleFuturePenalty);
}

search.run();

for (std::size_t i = 0; i < search.getNumberOfLoadsAdded(); ++i) {
    if (search.isLoadSelectedToBeOn(i)) {
        // search.getLoad(i) should be switched ON
    }
}
```

There is no artificial maximum number of Loads or branches — every
container is a `std::vector`.

## Candidate evaluation (Algorithm 4.3)

For each candidate added to the search:

```
p_i = P_i / max(P_available, 1 W)                              (4.27)
s_i = max(0, P_i_peak - P_i) / max(P_available, 1 W)            (4.28)
B   = clamp((SoC_warn - SoC) / (SoC_warn - SoC_min), 0, 1)      (4.29)
q_i = W_i / W_max                                                (4.31)

g(i) = w_P p_i + w_S s_i + w_B B                                 (4.30)
h(i) = w_Q (1 - q_i) + w_T r_i                                   (4.32)
f(i) = g(i) + h(i)                                                (4.33)
```

`P_available` here is `totalAvailablePowerWatts` — a constant for the
whole search, used only to normalize `p_i`/`s_i`. It is a genuinely
different quantity from `P_remaining` (`powerAvailableForAutoLoadsWatts`
at the start, then decreasing as candidates are admitted — see the
constraint guard below), whenever Fixed ON Loads have already committed
part of the total budget.

`P_i`/`P_i_peak` are `Load::getPower().runningWatts`/`.startupWatts`.
`W_i` is `Load::getPriority()`. `r_i` is supplied per candidate to
`addLoad()`, already prepared outside this class by
`LoadScheduleEvaluator` — `r_i = a_i(1 - d_i)`, zero for an unscheduled
Load or one whose schedule is already due, one for a Load whose schedule
is still in the future. `B` depends only on the search-wide battery state,
so it is computed once in `startSearch()`, not per candidate.

OPEN is a binary min-heap ordered by `f(i)`, so the candidate with the
smallest score is extracted first — lower running/startup power relative
to what's available, lower battery stress, higher priority, and a schedule
that has already arrived all push a candidate earlier in the search.

`addLoad()` rejects a candidate before scoring when its running power is
not a finite positive value, its startup power is less than its running
power, its priority exceeds `maximumAllowedPriority`, its
`scheduleFuturePenalty` is outside `[0, 1]`, or its `branchId` was never
registered with `registerBranch()`.

## Constraint guard (Algorithm 4.4 / Equation 4.25)

Each candidate extracted from OPEN is checked, in this exact order, before
it may be admitted:

```
1. SoC > SoC_min                                       -> else LOW_BATTERY
2. P_i <= P_remaining                                   -> else POWER_BUDGET_EXCEEDED
3. P_committed + P_i_peak <= P_battery,max               -> else BATTERY_CURRENT_LIMIT
4. (P_committed + P_i_peak) / V_B <= I_main,max          -> else MAIN_LIMIT_EXCEEDED
5. (P_branch + P_i_peak) / V_B <= I_branch,max           -> else BRANCH_LIMIT_EXCEEDED
```

```cpp
static constexpr std::uint8_t NONE = 0U;
static constexpr std::uint8_t LOW_BATTERY = 1U;
static constexpr std::uint8_t POWER_BUDGET_EXCEEDED = 2U;
static constexpr std::uint8_t BATTERY_CURRENT_LIMIT = 3U;
static constexpr std::uint8_t MAIN_LIMIT_EXCEEDED = 4U;
static constexpr std::uint8_t BRANCH_LIMIT_EXCEEDED = 5U;
```

A candidate that passes every condition is admitted (Algorithm 4.5,
Equations 4.34-4.37): it is marked selected, and `P_remaining`,
`P_committed` and its branch's committed power are all updated
immediately, before the next candidate is extracted — this is what makes
the allocation *sequential* rather than independent per candidate. A
rejected candidate consumes no power and its first failed condition is
stored, retrievable through `getLoadSelectionRejectionReason()`.

## Complexity

Every candidate is scored once (`O(n)`), inserted into the binary min-heap
once (`O(log n)` each, `O(n log n)` total) and extracted once
(`O(log n)` each, `O(n log n)` total); the constraint guard and admission
update are `O(1)` per candidate. Overall: `T(n) = O(n log n)`,
`S(n) = O(n)` (Section 4.6.3.6).

## Boundary

Sensor acquisition, battery State-of-Charge estimation, power-budget
calculation, Fixed-Load allocation, schedule evaluation, relay actuation,
ESP-NOW, Wi-Fi, MQTT and persistent storage do not belong in this
library. They supply inputs to the search (a battery/electrical state, a
set of registered branches, and a candidate list with each candidate's
branch and `r_i`) or consume its result (`isLoadSelectedToBeOn()`)
through separate modules:

- `LoadFilter` supplies the Auto candidates.
- `AvailablePowerManager` supplies `totalAvailablePowerWatts` (its Total
  Available Power), `powerAvailableForAutoLoadsWatts` (its Power
  Available for Auto Loads) and `initialCommittedPowerWatts` (its Fixed ON
  Running Power).
- `LoadScheduleEvaluator` supplies each candidate's `r_i`.
- `BatteryStateOfCharge` and `PowerBudgetCalculator` supply SoC, the SoC
  thresholds, `V_B` and the battery/main current limits.
- `RelayCommandDispatcher` (OFF-before-ON sequencing) and `RelayController`
  (local GPIO actuation) act on the result; `src/central/main.cpp`'s
  Optimisation Task wires the two together.
- What a `BranchId` corresponds to in the physical wiring is
  `{owning Node MAC address, relay pin}` — see `CentralNodeRegistry` for
  where each Branch's configuration (`I_branch,max`) actually comes from.
  candidates (Algorithm 4.2) — `BestFirstSearch` only tracks committed
  power against whatever `BranchId` it is told about.

## Dissertation mapping

- Problem formulation: Section 4.6.2, decision variables/SoC/available
  power/Fixed allocation/startup power/scheduling objective:
  Sections 4.6.2.1-4.6.2.6.
- Best-First Search theory and equations: Section 4.6.3.
- Candidate evaluation: Algorithm 4.3.
- Constraint checking: Algorithm 4.4.
- Min-heap scheduling / sequential allocation: Algorithm 4.5.
- Boundary preprocessing (Fixed allocation, candidate preparation) and
  relay actuation (Algorithm 4.2, Stage 1 and Algorithm 4.6, Stage 3) are
  external to this class, as described above.

These map to sections of the single `BestFirstSearch` class above
(`calculateAndStoreLoadScores()` / `checkFeasibility()` / the OPEN
min-heap methods / `markLoadSelectedToBeOn()`), not to four separate
files.
