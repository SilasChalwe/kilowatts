/**
 * @file RadioConfig.h
 * @brief The single Wi-Fi channel shared by ESP-NOW and Central's
 *        infrastructure Wi-Fi association.
 *
 * ESP-NOW (Smart Node <-> Central) and, on the Central Node only,
 * infrastructure Wi-Fi (used for MQTT and NTP) share one physical radio.
 * Every EspNowCommunication instance in this project must be constructed
 * with this same channel, and WiFiManager (Central only) must verify the
 * configured Access Point actually broadcasts on this channel before
 * associating with it — see WiFiManager's README for the
 * RADIO_CHANNEL_MISMATCH policy this constant exists to support.
 */

#ifndef KILOWATTS_RADIO_CONFIG_H
#define KILOWATTS_RADIO_CONFIG_H

#include <cstdint>

namespace kilowatts {

constexpr std::uint8_t KILOWATTS_RADIO_CHANNEL = 7U;

} // namespace kilowatts

#endif // KILOWATTS_RADIO_CONFIG_H
