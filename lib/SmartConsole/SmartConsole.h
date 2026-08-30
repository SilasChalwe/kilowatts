#ifndef KILOWATTS_SMART_CONSOLE_H
#define KILOWATTS_SMART_CONSOLE_H

#include "Load.h"
#include "SystemCommandModel.h"

#include <cstdint>

namespace kilowatts {

/** A Smart Node's own Load is always local - no target MAC to carry. */
struct SmartLoadConfigurationRequest {
    std::uint8_t relayPin;
    bool relayActiveHigh;
    char name[16];
    float powerRatingWatts;
    std::uint16_t priority;
    LoadPowerType powerType;
    LoadMode::Value mode;
    AutoSchedule schedule;
};

/** In-place priority/mode/schedule change - same idea as LoadCommandRequest, scoped to one local relay pin. */
struct SmartLoadUpdateRequest {
    std::uint8_t relayPin;

    bool hasPriority;
    std::uint16_t priority;

    bool hasMode;
    LoadMode::Value mode;

    bool hasSchedule;
    AutoSchedule schedule;
};

class SmartConsole {
public:
    struct Callbacks {
        void (*status)(void*);
        void (*loads)(void*);
        void (*loadStatus)(void*, std::uint8_t relayPin);

        CommandResult (*configureLoad)(void*, const SmartLoadConfigurationRequest&);
        CommandResult (*removeLoad)(void*, std::uint8_t relayPin);
        CommandResult (*updateLoad)(void*, const SmartLoadUpdateRequest&);

        void* context;
    };

    bool begin(const Callbacks& callbacks);

private:
    static int status(int argc, char** argv);
    static int loads(int argc, char** argv);
    static int load(int argc, char** argv);
    static int clear(int argc, char** argv);

    static SmartConsole* active_;
    Callbacks callbacks_{};
};

} // namespace kilowatts

#endif // KILOWATTS_SMART_CONSOLE_H
