# LoadScheduleEvaluator

Evaluates an Auto Load's configured schedule against real local time and
produces the schedule terms Chapter 4 defines — a_i, d_i and the
future-schedule penalty r_i (Section 4.6.3.3 / Equation 4.32) — for
`BestFirstSearch` to consume as one input to h(i). It is the last
preparation step before Load selection.

## Responsibility

```cpp
LoadScheduleEvaluator scheduleEvaluator;

LoadScheduleEvaluation result{};
if (scheduleEvaluator.evaluateSchedule(autoLoad, currentTimeProvider, result)) {
    // result.hasEnabledSchedule  -> a_i
    // result.isScheduledTimeDue  -> d_i
    // result.futureSchedulePenalty -> r_i, ready to hand to BestFirstSearch::addLoad()
}
```

- **Not an Auto Load** (`Fixed::ON`/`Fixed::OFF`) → `evaluateSchedule()`
  returns `false`. A schedule never overrides Fixed Load semantics, and a
  Fixed Load is never converted into an Auto Load here.
- **No schedule enabled** → `a_i = false`, `d_i = false`,
  `r_i = 0.0F`.
- **Schedule enabled but not yet due**, including when the clock is not
  currently synchronized (an unsynchronized clock can never confirm a
  schedule is due — it is treated exactly like "not yet due", never
  fabricated as "due") → `a_i = true`, `d_i = false`, `r_i = 1.0F`.
- **Schedule enabled and due** (current local hour/minute at or after the
  scheduled hour/minute) → `a_i = true`, `d_i = true`, `r_i = 0.0F`.

`r_i = a_i(1 - d_i)` exactly matches Equation 4.32. It is a schedule
penalty, not a priority — this class never reads or produces a priority
value. `Load::getPriority()` (`W_i`) is a completely separate term that
`BestFirstSearch` normalizes and weighs on its own (`q_i`, `w_Q`); the two
are only ever combined together inside
`BestFirstSearch::calculateHeuristicCost()`, never here. A Load's own
stored priority is therefore never read or modified by this class.

`AutoSchedule` (`enabled`, `hour`, `minute` — see `Load.h`) has no
duration or day-of-week field, so "due" means exactly what those three
fields can express: local time has reached or passed the scheduled
hour:minute for the current day. No seconds, weekdays, date ranges,
duration, calendar recurrence or timezone fields are invented here —
timezone handling belongs to `CurrentTimeProvider`, not this class.

## Testing

This module cannot be host-tested through `run_cpp_test.sh`: it calls
`CurrentTimeProvider`'s real methods, whose implementations require
ESP-IDF's SNTP/NVS headers and only exist in `CurrentTimeProvider.cpp`,
which is not compiled by a `LoadScheduleEvaluator`-scoped test run.
Schedule behavior must be verified on real ESP32 hardware with real
current time. `BestFirstSearch`'s own host test instead exercises how a
supplied `r_i` value feeds into h(i) directly, using controlled float
inputs rather than a real `LoadScheduleEvaluator`/`CurrentTimeProvider`
pair.

`CurrentTimeProvider`'s Manual mode (see its own README) is a real
production feature, not a test backdoor, but it does make on-device
verification practical without waiting on internet connectivity: put the
device in Manual mode with a real, deliberately chosen date/time
(`setTimeMode(TimeMode::MANUAL)` + `setManualCurrentDateTime()`), then
construct Auto Loads with schedules (via `Load::setSchedule()`) before,
equal to, and after that chosen time, and confirm `evaluateSchedule()`
reports `a_i=true, d_i=false, r_i=1.0F` for the "not yet due" cases and
`a_i=true, d_i=true, r_i=0.0F` for the "due" case — for both `Auto::ON`
and `Auto::OFF` — and that a `Fixed::ON` or `Fixed::OFF` Load with the
same schedule fields set is rejected (`false`) rather than evaluated. The
same check can be repeated in Automatic mode once a real NTP
synchronization has completed. Either way, `LoadScheduleEvaluator` itself
is never modified or given a fake clock to make this pass — only
`CurrentTimeProvider`'s own real, source-agnostic time is used.

## Boundaries

`LoadScheduleEvaluator` does not own the clock (`CurrentTimeProvider`
does), does not implement NTP itself, does not modify the system clock,
does not read INA219, does not run Best-First Search, and does not read or
modify a Load's stored priority. It only prepares the schedule terms
`BestFirstSearch` will eventually consume.
