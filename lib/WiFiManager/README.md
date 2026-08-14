# WiFiManager

Central Node infrastructure Wi-Fi station connectivity, used only so
`MqttManager` and `CurrentTimeProvider`'s Automatic (NTP) mode can reach
the internet. Smart Nodes never construct this class — they have no
Wi-Fi/MQTT session of their own (see `EspNowCommunication`'s README);
only the Central Node bridges local ESP-NOW to the wider network.

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

```cpp
WiFiManager wifi(kilowatts::KILOWATTS_RADIO_CHANNEL);
wifi.begin(WiFiManager::Credentials{Secrets::WIFI_SSID, Secrets::WIFI_PASSWORD});

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
relays. Wi-Fi/MQTT credentials live in `include/KilowattsSecrets.h`
(gitignored; see `KilowattsSecrets.h.example`) — never in this module.

## Host build

This module requires the real ESP-IDF Wi-Fi/event stack and is therefore
ESP32-target-only, like `EspNowCommunication` and `ChipInfo` — it has no
host build split and no host-native test.
