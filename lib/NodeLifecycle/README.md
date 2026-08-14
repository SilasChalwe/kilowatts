# NodeLifecycle

The device commissioning lifecycle shared by both Kilowatts firmware roles —
header + a small `.cpp` of pure transition-validation logic, no wire format
and no persistence of its own.

## Responsibility

```cpp
NodeLifecycleState state = NodeLifecycleState::UNCOMMISSIONED;

if (isValidNodeLifecycleTransition(state, NodeLifecycleState::DISCOVERED)) {
    state = NodeLifecycleState::DISCOVERED;
}

toText(state);   // "discovered" - for logs/JSON
```

```
FACTORY -> UNCOMMISSIONED
UNCOMMISSIONED -> DISCOVERED | CONFIGURING | DECOMMISSIONED
DISCOVERED -> CONFIGURING | DECOMMISSIONED
CONFIGURING -> COMMISSIONED | UNCOMMISSIONED | DECOMMISSIONED
COMMISSIONED -> OPERATIONAL | DECOMMISSIONED
OPERATIONAL -> DECOMMISSIONED
DECOMMISSIONED -> UNCOMMISSIONED
```

A newly flashed device with no valid persisted commissioning record boots
`UNCOMMISSIONED` (never `FACTORY` beyond the instant before that first check
resolves it). `CONFIGURING` is only ever entered from `UNCOMMISSIONED`/
`DISCOVERED` for a Node's *first* commissioning — renaming an already-
`COMMISSIONED`/`OPERATIONAL` Node never revisits `CONFIGURING` (see
`NodeCommissioningRegistry::rename()`), so a failed/timed-out commissioning
attempt can unambiguously roll back to `UNCOMMISSIONED` without ever
un-commissioning a Node that was already working. `OPERATIONAL` is reserved
for a later phase once a Node can actually own Branches/Loads; nothing in
this phase produces it.

`isValidNodeLifecycleTransition()` never treats a state as validly
transitioning to itself — a caller that only needs to refresh bookkeeping
(for example "last seen" timestamps) for an already-known state should not
call it at all.

`MacAddress` here is the same six-byte layout `Load::MacAddress` and
`EspNowCommunication::MacAddress` already use, declared independently
(rather than included from either) so the commissioning libraries that
build on this header stay free of any ESP-IDF or domain-object dependency
and remain host-testable.

## Boundaries

`NodeLifecycle` only defines the state values and which transitions between
them are structurally legal. It does not decide *when* a transition should
happen (`NodeCommissioningRegistry` on Central, `NodeIdentityStore` on a
Smart Node), does not perform ESP-NOW communication
(`CommissioningPackets`/`EspNowCommunication`), and does not persist
anything.
