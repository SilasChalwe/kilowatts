# LoadFilter

Separates every Load Central currently knows about into Fixed ON, Fixed
OFF, and Auto candidate groups — the preparation step that runs before
Best-First Search. Classification is its only responsibility.

## Responsibility

```cpp
LoadFilter loadFilter;

for (/* every Load known by CentralNodeRegistry */) {
    loadFilter.addLoad(load);
}

loadFilter.getNumberOfFixedOnLoads();
loadFilter.getFixedOnLoad(i);

loadFilter.getNumberOfAutoCandidateLoads();
loadFilter.getAutoCandidateLoad(i);         // BestFirstSearch decides ON/OFF for these
```

`addLoad()` classifies by the Load's current `LoadMode`:

| Mode | Goes to |
|---|---|
| `Fixed::ON` | Fixed ON collection |
| `Fixed::OFF` | Fixed OFF collection |
| `Auto::ON` or `Auto::OFF` | Auto candidate collection |

A Fixed Load's ON/OFF state is authoritative and is not reconsidered here.
An Auto Load's current ON/OFF state is only this planning cycle's starting
point — `AUTO_ON` and `AUTO_OFF` are therefore both Auto *candidates*;
whether each ends up ON or OFF is decided later by `BestFirstSearch`, not
by `LoadFilter`.

`LoadFilter` stores **pointers** to the existing `Load` objects, not
copies, so every Load keeps the identity `CentralNodeRegistry` already
established. A `LoadFilter` must not outlive the Loads it points to.
Call `reset()` before re-traversing the registry (for example after a new
`NODE_REPORT`) so a fresh pass never mixes in Loads left over from an
earlier classification.

## Boundaries

`LoadFilter` classifies Loads only. It does not calculate Fixed ON Running
Power, Total Available Power, or Power Available for Auto Loads — that
power accounting now belongs to `AvailablePowerManager`, which receives an
already-classified `LoadFilter` and traverses its Fixed ON collection.
`LoadFilter` also does not discover Nodes or receive ESP-NOW packets
(`EspNowCommunication`), does not calculate RSSI or Hop Count, does not
calculate battery State of Charge, does not run Best-First Search, does
not control relays, and does not publish MQTT.
