# FirmwareManager

General firmware infrastructure that is not specific to any single
Node's identity, commissioning or role — hardware/chip diagnostics
(ChipInfo) and wall-clock time management (CurrentTimeProvider).
Grouped by domain; each class still strictly single-responsibility
per its own section below.

## ChipInfo


Reads real ESP32 chip and hardware information directly from ESP-IDF —
target, silicon revision, Wi-Fi/Bluetooth features, flash size, free heap,
PSRAM, ESP-IDF version, and the Wi-Fi station MAC address.

## Responsibility

`ChipInfo` is a small, self-contained wrapper around ESP-IDF's system
information APIs (`esp_chip_info`, `esp_flash_get_physical_size`,
`esp_get_free_heap_size`, `esp_psram_is_initialized`,
`esp_get_idf_version`, `esp_read_mac`). It has exactly two jobs:

```cpp
ChipInfo chipInfo;
chipInfo.printAll();               // logs a full system-information block

ChipInfo::MacAddress macAddress{};
if (chipInfo.getMacAddress(macAddress)) {
    // macAddress[0..5] is this ESP32's real Wi-Fi station MAC address
}
```

`getMacAddress()` is the one method other modules actually depend on:
`EspNowCommunication` reads the local Node's real MAC address through it
rather than duplicating `esp_read_mac()` itself, so there is exactly one
place in the codebase that knows how to obtain the hardware MAC address.

`getChipModelText()` reuses the same `esp_chip_info()` read `printChip()`
already performs, formatted as a short stable identity string (target + CPU
core count, e.g. `"esp32:2core"`) rather than logged text — the commissioning
identity report (`CommissioningPackets::IdentityReportPacket`) uses it so
Central can record what kind of chip a Node is without a second, duplicate
`esp_chip_info()` call anywhere else in the codebase.

## Boundaries

`ChipInfo` does not perform ESP-NOW communication, does not know about
Nodes or Loads, and does not cache or interpret the MAC address beyond
returning the six raw bytes — it is a thin, honest read of real hardware
state, not a domain object.

---


---

## CurrentTimeProvider


Real wall-clock time for the Kilowatts firmware, from exactly one of two
legitimate production sources the user selects — not a fake or forced
test clock.

## Two Time Modes

```cpp
enum class TimeMode : std::uint8_t { AUTOMATIC = 0U, MANUAL = 1U };
```

- **AUTOMATIC** — the existing real ESP-IDF SNTP client synchronizes the
  ESP32 system clock from a real NTP server whenever internet/IP
  connectivity is available.
- **MANUAL** — the user/application supplies a real current local
  date/time directly, and that value becomes authoritative even while
  internet connectivity exists. This is a real production fallback for
  when internet time is unavailable or the user deliberately wants
  manual control — not a test-only affordance.

Both modes ultimately set the **same** ESP32 system clock. There are
never two independent clocks, and there is no custom incrementing timer
loop.

## Three distinct concepts

```cpp
enum class TimeSource : std::uint8_t { NONE = 0U, NTP = 1U, MANUAL = 2U };
```

| Concept | Meaning | Where it comes from |
|---|---|---|
| **Time Mode** | What the user selected | `getTimeMode()`/`setTimeMode()`, persisted in NVS |
| **Time Source** | What actually established the *currently valid* clock right now | `getCurrentTimeSource()` |
| **Time Validity** | Whether current time can be trusted right now | `isCurrentTimeValid()` — exactly `getCurrentTimeSource() != TimeSource::NONE` |

These are deliberately never collapsed into one boolean. The state model:

| Mode | Source establishing the clock | Validity |
|---|---|---|
| AUTOMATIC | no NTP sync yet | invalid |
| AUTOMATIC | successful NTP sync occurred | valid, Source = NTP |
| MANUAL | no manual time entered yet (this boot) | invalid |
| MANUAL | valid manual time entered | valid, Source = MANUAL |

**Changing Time Mode always resets Time Source to `NONE`** — a value
established under the previous mode is never carried over and silently
kept "valid" under the new one. The underlying system clock *value* is
left untouched by a mode change; only this object's own trust bookkeeping
resets, until the newly selected mode's own mechanism (a real sync, or a
fresh manual entry) re-establishes it.

## Automatic mode

Unchanged from the original real implementation: `setTimeMode` /
`initializeTimeSynchronization()` start ESP-IDF's SNTP client
(`esp_netif_sntp_init()`), which synchronizes the ESP32 system clock
whenever IP connectivity exists, and continues reading from that system
clock afterward — never a fresh internet request per read, and the clock
keeps advancing locally if internet is later lost. See "ESP-IDF API used"
below for the exact calls.

## Manual mode

```cpp
struct ManualDateTime {
    std::uint16_t year;
    std::uint8_t month;
    std::uint8_t day;
    std::uint8_t hour;
    std::uint8_t minute;
    std::uint8_t second;
};

currentTimeProvider.setTimeMode(TimeMode::MANUAL);   // also stops SNTP
currentTimeProvider.setManualCurrentDateTime(ManualDateTime{2026, 8, 13, 14, 30, 0});
```

A full date is required, not just hour/minute — after a reboot or a date
rollover the system needs the actual date, not only "today's" time.
Every field is validated and **rejected rather than silently normalized**:
an out-of-range month/day/hour/minute/second (including an impossible
date like 30 February, correctly checked against real days-per-month and
leap years) is refused with no change to the system clock or Time Source.

A valid entry is converted with `mktime()` (interpreted as **local**
time, using the timezone already configured — see Timezone below) and
applied to the real ESP32 system clock with `settimeofday()`. Time Source
becomes `MANUAL`.

**Manual overrides NTP**: `setTimeMode(TimeMode::MANUAL)` stops SNTP
(`esp_netif_sntp_deinit()`) so it cannot silently overwrite a manually
supplied value, and the SNTP sync callback itself additionally checks the
current Time Mode and ignores a late/in-flight synchronization event that
arrives after the mode has already changed away from `AUTOMATIC` — so
even a race between "user switches to Manual" and "an SNTP request
already in flight completes" cannot clobber the manual value. Manual time
remains authoritative until the user explicitly calls
`setTimeMode(TimeMode::AUTOMATIC)`; internet connectivity returning does
not do this automatically.

## Switching back to Automatic

`setTimeMode(TimeMode::AUTOMATIC)` resumes SNTP. Time Source is reset to
`NONE` (invalid) immediately, exactly like every other mode change, and
only becomes `NTP` once a real synchronization actually completes
afterward — NTP synchronization is never claimed in advance.

## Persistence (NVS)

Time Mode is persisted so a reboot remembers the user's selection — using
the real ESP-IDF NVS API directly (persistent non-volatile configuration
storage, not a database), narrowly scoped to this one responsibility:

- Namespace: `kw_time`
- Key `mode` (`u8`): the persisted `TimeMode` (defaults to `AUTOMATIC`
  the first time a Node ever runs, or if the key is missing/unreadable).
- Key `manual_ts` (`i64`): the epoch seconds of the **last** manually
  entered date/time, kept only as a recovery/reference value — see
  `getLastConfiguredManualDateTime()`.

**A flash-stored timestamp does not advance while the ESP32 is completely
powered off.** `getLastConfiguredManualDateTime()` exposes that stored
value purely for reference/diagnostic display — it is never treated as
current valid time, and neither `isCurrentTimeValid()` nor
`getCurrentLocalDateTime()` will report it as such. After any boot:
- **Automatic**: current time is invalid until a real NTP sync completes
  this boot.
- **Manual**: current time is invalid until `setManualCurrentDateTime()`
  is called this boot, even though the *mode* itself is remembered.

No external RTC is added in this phase, and no persistence capability the
hardware does not provide is assumed or invented.

## ESP-IDF API used

- **Automatic**: the modern, thread-safe **esp_netif SNTP** integration —
  `esp_netif_sntp_init()` / `esp_netif_sntp_deinit()` from
  `driver/esp_netif_sntp.h` (ESP-IDF v5.5.4, `esp_netif` component). Not
  the raw `apps/esp_sntp.h` polling API, not a hand-rolled NTP
  packet implementation, and not an Arduino NTP library.
- **Manual**: standard POSIX/ESP-IDF `mktime()` (local-time fields →
  epoch) and `settimeofday()` (applies to the real system clock) —
  the same system clock Automatic mode uses, not a second one.
- **Persistence**: `nvs_flash_init()` (with the same erase-and-retry
  pattern on `ESP_ERR_NVS_NO_FREE_PAGES`/`ESP_ERR_NVS_NEW_VERSION_FOUND`
  already used by `EspNowCommunication`), `nvs_open()`, `nvs_get_u8()` /
  `nvs_set_u8()`, `nvs_get_i64()` / `nvs_set_i64()`, `nvs_commit()`,
  `nvs_close()`.
- **NTP server**: `pool.ntp.org` — the standard public NTP pool used
  throughout ESP-IDF's own SNTP documentation and examples.
- **Timezone**: `TZ=CAT-2` (POSIX TZ string) + `tzset()`. CAT (Central
  Africa Time, UTC+2, no daylight saving) matches this project's stated
  institution, The Copperbelt University, Zambia. ESP-IDF's newlib does
  not carry the IANA zoneinfo database, so a POSIX TZ string is used
  rather than a zone name like `Africa/Lusaka`. **If the physical
  deployment location is not Zambia, this constant needs updating** — a
  documented assumption based on project context, not a guaranteed
  physical fact. Timezone conversion stays entirely inside this class —
  `LoadScheduleEvaluator` never performs it, and a Manual entry is
  interpreted as local time using this same configured timezone.

## Internet connectivity and Automatic mode

Automatic mode still needs a network interface with a real
internet-routed IP address to synchronize against. On the **Central**
Node this now exists: `WiFiManager` associates with the installation's
own Access Point — station credentials read from `WiFiCredentialsStore`,
or captured for the first time through `WiFiProvisioningPortal`'s
captive portal when none are saved — after
`EspNowCommunication::initialize()` brings the radio up in station mode,
and reports `CONNECTED_WITH_IP` once a real IP address is obtained (see
`src/central/main.cpp` and the `WiFiManager` README). Only once Central
reaches that state can `esp_netif_sntp_init()` actually reach a real NTP
server, so Automatic mode on Central is a genuinely production-legitimate
path today, not only Manual mode.

**Smart Nodes have no such path.** `WiFiManager` is deliberately
Central-only — a Smart Node's radio stays in ESP-NOW station mode and
never associates with an Access Point (see `WiFiManager`'s own boundary
notes) — so Automatic mode on a Smart Node still cannot synchronize.
Manual mode remains the only production-legitimate way for a Smart Node
to have valid time, until a time-distribution mechanism over ESP-NOW
(Central relaying its own synced time down to Smart Nodes) is added. No
credentials have been invented here to work around that gap.

## On-device diagnostic path

`printDiagnosticReport()` logs Time Mode, Time Source, current validity,
SNTP running status (while in Automatic mode), and — only when valid —
current local time, current UTC time, local hour and local minute. It
never prints a fabricated date/time when invalid; instead it reports
`WAITING FOR INTERNET TIME` (Automatic) or `WAITING FOR MANUAL TIME
ENTRY` (Manual). To verify on real hardware:
- **Automatic + internet available**: call `initializeTimeSynchronization()`
  once, then `printDiagnosticReport()` more than once over time to
  confirm Source becomes `NTP`, Validity becomes `Yes`, and the reported
  time advances between calls without a fresh synchronization each time.
- **Manual**: call `setTimeMode(TimeMode::MANUAL)`, then
  `setManualCurrentDateTime()` with a real date/time, then
  `printDiagnosticReport()` to confirm Source `MANUAL`, Validity `Yes`,
  and that the reported time keeps advancing on its own afterward (same
  system clock, not re-read from the manual entry each time).
- **Manual overriding Automatic**: while internet is available and
  Automatic has already synchronized, switch to Manual with a
  deliberately different time and confirm it sticks (Source stays
  `MANUAL`) rather than being overwritten by a subsequent SNTP sync.

## Boundaries

`CurrentTimeProvider` does not know about Loads, schedules, effective
priority, power, ESP-NOW, or relay control — see `LoadScheduleEvaluator`
for the module that consumes this one's local time without caring which
mode/source established it.
