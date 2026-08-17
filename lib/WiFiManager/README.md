# WiFiManager

Everything the Central Node needs for infrastructure Wi-Fi lives in this
one library, as three separate single-responsibility classes sharing a
folder rather than one merged class:

- **`WiFiManager`** — station connectivity itself (this file's original
  subject; see below).
- **`WiFiCredentialsStore`** — persists an installer-provisioned
  SSID/password to NVS, so a site's real Wi-Fi survives reboots and
  factory-reset-free re-flashing. Knows nothing about *when* to load/save
  or about station connectivity itself.
- **`WiFiProvisioningPortal`** — the self-hosted "Kilowatts-Setup-XXXX"
  Access Point and captive-portal HTTP form an installer uses to submit
  that SSID/password from a phone when no credentials are provisioned yet
  or the provisioned ones stop working. Does not manage station
  connectivity and does not decide when to start/stop — `src/central/main.cpp`
  owns that policy (immediately at boot with no provisioned credentials;
  via `checkWiFiProvisioningTrigger()` in the watchdog task after repeated
  reconnect failures once credentials are provisioned but stop working).

Used only so `MqttManager` and `CurrentTimeProvider`'s Automatic (NTP)
mode can reach the internet. Smart Nodes never construct any of these
classes — they have no Wi-Fi/MQTT session of their own (see
`EspNowCommunication`'s README); only the Central Node bridges local
ESP-NOW to the wider network.

## Radio channel coexistence

ESP-NOW and infrastructure Wi-Fi share one physical radio.
`EspNowCommunication::initialize()` already brings the Wi-Fi driver up in
station mode on `kilowatts::KILOWATTS_RADIO_CHANNEL` (see
`include/RadioConfig.h`) as a prerequisite for ESP-NOW — `WiFiManager`
never calls `esp_wifi_init()`/`esp_wifi_start()`/`esp_wifi_set_mode()`
itself, only `begin()` after `EspNowCommunication::initialize()` has
already succeeded.

Before ever calling `esp_wifi_connect()`, `WiFiManager` scans for the
configured SSID and checks the channel it is actually broadcasting on.
Connecting to an Access Point on a different channel would force the
whole shared radio onto that channel and silently break ESP-NOW to every
Smart Node, so a mismatch is refused outright:

```
RADIO_CHANNEL_MISMATCH: SSID 'X' is broadcasting on channel 6 but
ESP-NOW requires channel 1. Reconfigure the Access Point onto the
required channel — local ESP-NOW control is unaffected, but MQTT/Wi-Fi
connectivity stays unavailable until this is corrected.
```

Local ESP-NOW sensing/relay control is completely unaffected by this —
only MQTT/Wi-Fi connectivity stays unavailable until the Access Point is
reconfigured onto `KILOWATTS_RADIO_CHANNEL`.

## Usage

There is no compiled-in factory-default SSID/password — a never-provisioned
device does not know any site's Wi-Fi. `src/central/main.cpp` boots
straight into `WiFiProvisioningPortal` when `WiFiCredentialsStore::load()`
finds nothing, and only calls `WiFiManager::begin()` once real credentials
exist:

```cpp
WiFiManager wifi(kilowatts::KILOWATTS_RADIO_CHANNEL);
WiFiCredentialsStore credentialsStore;
WiFiProvisioningPortal portal;

WiFiCredentialsStore::Credentials credentials{};
if (credentialsStore.load(credentials)) {
    wifi.begin(WiFiManager::Credentials{credentials.ssid, credentials.password, hostname});
} else {
    portal.begin(kilowatts::KILOWATTS_RADIO_CHANNEL);
}

if (wifi.isConnected()) {
    // safe to start MqttManager / rely on CurrentTimeProvider's Automatic mode
}
```

`getState()` reports `DISCONNECTED`, `SCANNING`, `CONNECTING`,
`CONNECTED_AWAITING_IP`, `CONNECTED_WITH_IP` or `RADIO_CHANNEL_MISMATCH`
— never collapsed into a single boolean, since the MQTT system-state
topic (see `MqttManager`) needs to tell these apart for the mobile
application. A dropped connection (`WIFI_EVENT_STA_DISCONNECTED`)
schedules a reconnection attempt with exponential backoff (2s, 4s, 8s,
... capped at 60s), re-verifying the channel every time — a channel
mismatch is never assumed to be a transient failure that will fix itself.

## Boundary

`WiFiManager` owns Wi-Fi station connectivity only. It does not perform
MQTT (`MqttManager` owns the broker connection), does not perform NTP
itself (`CurrentTimeProvider` uses ESP-IDF's SNTP client once an IP
address exists), and knows nothing about Loads, Best-First Search, or
relays. Wi-Fi credentials are never compiled in — they only ever come
from `WiFiCredentialsStore` (NVS, populated by `WiFiProvisioningPortal`).
MQTT broker credentials still live in `include/KilowattsSecrets.h`
(gitignored; see `KilowattsSecrets.h.example`), which is unrelated to
Wi-Fi station credentials.

## Host build

`WiFiManager` and `WiFiProvisioningPortal` require the real ESP-IDF
Wi-Fi/event/HTTP-server stack and are therefore ESP32-target-only, like
`EspNowCommunication` and `ChipInfo` — no host build split, no
host-native test. `WiFiCredentialsStore`'s NVS persistence is likewise
ESP32-only, matching `CurrentTimeProvider` and `INA219Monitor`'s
calibration storage split.
