# LoadManager

`LoadManager` contains the domain model used by Kilowatts planning.

## Load model

A `Load` represents one configured electrical load owned by a Node. Its identity is the owning Node MAC address plus the configured GPIO/control pin (`relayPin` in the current API).

The current Load data is:

- Node MAC + control pin identity,
- name,
- `powerRatingWatts`,
- priority,
- `LoadPowerType` (`AC` or `DC`),
- mode (`FIXED_OFF`, `FIXED_ON`, `AUTO_OFF`, `AUTO_ON`),
- optional `AutoSchedule`,
- last Best-First rejection reason for diagnostics.

There are no per-Load `startupWatts`, nominal-voltage, nominal-current, target-state, applied-state, or confirmed-relay-state fields.

## `powerRatingWatts`

`powerRatingWatts` is the configured expected operating power of the Load while it is ON. It is normally entered during installation.

Example:

```text
Fan  = 25 W
TV   = 60 W
Lamp = 10 W
```

The planner treats those values as the expected ON consumption. They are not live per-Load sensor readings. Battery-side live voltage/current/power are measured separately by `PowerManager` (or supplied by simulation).

## Modes

`FIXED_ON` and `FIXED_OFF` are authoritative user configuration and are not Best-First Search candidates.

`AUTO_ON` and `AUTO_OFF` identify loads that may be considered by the automatic planner. Best-First Search does not rewrite the configured mode when it selects or rejects a Load.

## Planning flow

```text
all Loads
   |
   v
LoadFilter
   +-- FIXED_ON  -> contributes committed/fixed power
   +-- FIXED_OFF
   +-- AUTO candidates -> BestFirstSearch
```

The runtime computes the desired GPIO command directly from fixed mode plus the Best-First selected AUTO combination. `Load` does not store a fake hardware state.
