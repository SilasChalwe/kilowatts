#ifndef KILOWATTS_CENTRAL_CONSOLE_H
#define KILOWATTS_CENTRAL_CONSOLE_H

#include "Load.h"
#include "MqttCredentialsStore.h"
#include "SystemCommandModel.h"
#include "WiFiCredentialsStore.h"

#include <cstdint>

namespace kilowatts {

struct BatterySensorCommandRequest {
    float shuntResistanceOhms;
    float maximumExpectedCurrentAmps;
    float emaAlpha;
    float batteryCapacityAmpHours;
    float initialStateOfChargePercent;
    float nominalVoltageVolts;
};

/** requiredRuntimeHours: 0 = no target configured. */
struct PowerLimitsCommandRequest {
    float minimumStateOfChargePercent;
    float maximumBatteryDischargeCurrentAmps;
    float maximumMainCurrentAmps;
    float requiredRuntimeHours;
};

struct NodeCommandRequest {
    enum class Action : std::uint8_t {
        COMMISSION = 0U,
        RENAME = 1U,
        DECOMMISSION = 2U
    } action;

    Load::MacAddress nodeMacAddress;
    char friendlyName[20];
};

/** No startup watts, nominal volts, nominal amps, or other invented fields. */
struct LoadConfigurationCommandRequest {
    Load::MacAddress nodeMacAddress;
    std::uint8_t relayPin;
    bool relayActiveHigh;
    char name[16];
    float powerRatingWatts;
    std::uint16_t priority;
    LoadPowerType powerType;
    LoadMode::Value mode;
    AutoSchedule schedule;
};

struct RemoveLoadCommandRequest {
    Load::MacAddress nodeMacAddress;
    std::uint8_t relayPin;
};

enum class NetworkCommandTarget : std::uint8_t {
    WIFI = 0U,
    MQTT = 1U
};

struct NetworkCommandRequest {
    NetworkCommandTarget target;

    enum class Action : std::uint8_t {
        STATUS = 0U,
        SET = 1U,
        CLEAR = 2U,
        SETUP = 3U,
        SCAN = 4U,
        SET_CHANNEL = 5U
    } action;

    char ssid[WiFiCredentialsStore::SSID_BUFFER_SIZE];
    char wifiPassword[WiFiCredentialsStore::PASSWORD_BUFFER_SIZE];
    std::uint8_t wifiChannel;

    char mqttHost[MqttCredentialsStore::HOST_BUFFER_SIZE];
    std::uint16_t mqttPort;
    bool mqttUseTls;
    char mqttUsername[MqttCredentialsStore::USERNAME_BUFFER_SIZE];
    char mqttPassword[MqttCredentialsStore::PASSWORD_BUFFER_SIZE];
};

class CentralConsole {
public:
    struct Callbacks {
        void (*status)(void*);
        void (*dashboard)(void*);
        void (*batteryStatus)(void*);

        void (*nodes)(void*);
        void (*nodeStatus)(void*, const Load::MacAddress&);
        void (*loads)(void*);
        void (*loadStatus)(void*, const Load::MacAddress&, std::uint8_t relayPin);

        void (*optimize)(void*);

        bool (*sensorMode)(void*, bool simulated);

        Load::MacAddress (*localMac)(void*);

        CommandResult (*configureBattery)(
            void*, const BatterySensorCommandRequest&);

        CommandResult (*configurePowerLimits)(
            void*, const PowerLimitsCommandRequest&);

        CommandResult (*nodeCommand)(
            void*, const NodeCommandRequest&);

        CommandResult (*configureLoad)(
            void*, const LoadConfigurationCommandRequest&);

        CommandResult (*removeLoad)(
            void*, const RemoveLoadCommandRequest&);

        /**
         * Uses the SAME canonical LoadCommandRequest and the SAME handler
         * MQTT's commands/load topic drives — see SystemCommandModel.h.
         */
        CommandResult (*loadCommand)(
            void*, const LoadCommandRequest&);

        CommandResult (*network)(
            void*, const NetworkCommandRequest&);

        CommandResult (*simulation)(
            void*, const SimulationCommandRequest&);

        CommandResult (*system)(
            void*, const SystemCommandRequest&);

        void* context;
    };

    bool begin(const Callbacks& callbacks);

private:
    static int status(int argc, char** argv);
    static int dashboard(int argc, char** argv);
    static int battery(int argc, char** argv);
    static int sensor(int argc, char** argv);
    static int nodes(int argc, char** argv);
    static int node(int argc, char** argv);
    static int loads(int argc, char** argv);
    static int load(int argc, char** argv);
    static int optimize(int argc, char** argv);
    static int wifi(int argc, char** argv);
    static int mqtt(int argc, char** argv);
    static int simulation(int argc, char** argv);
    static int system(int argc, char** argv);
    static int clear(int argc, char** argv);

    static CentralConsole* active_;
    Callbacks callbacks_{};
};

} // namespace kilowatts

#endif // KILOWATTS_CENTRAL_CONSOLE_H