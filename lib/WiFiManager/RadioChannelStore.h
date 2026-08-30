/**
 * RadioChannelStore only reads and writes the saved channel byte; it does
 * not apply the channel to the radio itself
 * (EspNowCommunication::setChannel(), WiFiManager::setRequiredChannel())
 * and does not decide when a restart is required.
 */

#ifndef KILOWATTS_RADIO_CHANNEL_STORE_H
#define KILOWATTS_RADIO_CHANNEL_STORE_H

#include <cstdint>

namespace kilowatts {


class RadioChannelStore {

public:

    /**
     * Loads a previously-saved channel override into channel. Returns
     * false (channel left untouched) on a host build, or when nothing has
     * ever been saved.
     */
    bool load(std::uint8_t& channel) const;


    /**
     * Persists channel, overwriting whatever was saved before. Returns
     * false when channel is 0 or greater than 14 (not a valid 2.4 GHz
     * channel number), or the underlying NVS write failed (ESP32 target
     * only - always false on a host build).
     */
    bool save(std::uint8_t channel) const;


    /** Erases any saved override; the next boot uses the compiled-in default. */
    bool clear() const;
};


} // namespace kilowatts

#endif // KILOWATTS_RADIO_CHANNEL_STORE_H
