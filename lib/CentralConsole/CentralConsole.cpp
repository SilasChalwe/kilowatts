#include "CentralConsole.h"

#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace kilowatts {
namespace {

constexpr const char* TAG = "CENTRAL_CONSOLE";

bool same(const char* first, const char* second)
{
    return first != nullptr &&
           second != nullptr &&
           std::strcmp(first, second) == 0;
}

bool helpRequested(int argc, char** argv)
{
    return argc == 2 &&
           (same(argv[1], "help") ||
            same(argv[1], "--help") ||
            same(argv[1], "-h"));
}

const char* option(int argc, char** argv, const char* name)
{
    if (name == nullptr) return nullptr;

    const std::size_t length = std::strlen(name);
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == nullptr) continue;
        if (std::strncmp(argv[i], name, length) == 0 &&
            argv[i][length] == '=') {
            return argv[i] + length + 1U;
        }
    }
    return nullptr;
}

bool copyText(
    char* destination,
    std::size_t destinationSize,
    const char* source)
{
    if (destination == nullptr ||
        destinationSize == 0U ||
        source == nullptr ||
        std::strlen(source) >= destinationSize) {
        return false;
    }

    std::snprintf(destination, destinationSize, "%s", source);
    return true;
}

bool parseFloat(const char* text, float& value)
{
    if (text == nullptr || *text == '\0') return false;

    char* end = nullptr;
    const float parsed = std::strtof(text, &end);

    if (end == text || *end != '\0' || !std::isfinite(parsed)) {
        return false;
    }

    value = parsed;
    return true;
}

bool parseUnsigned(const char* text, unsigned long& value)
{
    if (text == nullptr || *text == '\0') return false;

    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0') return false;

    value = parsed;
    return true;
}

bool parseUint8(const char* text, std::uint8_t& value)
{
    unsigned long parsed = 0U;
    if (!parseUnsigned(text, parsed) ||
        parsed > std::numeric_limits<std::uint8_t>::max()) {
        return false;
    }

    value = static_cast<std::uint8_t>(parsed);
    return true;
}

bool parseUint16(const char* text, std::uint16_t& value)
{
    unsigned long parsed = 0U;
    if (!parseUnsigned(text, parsed) ||
        parsed > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }

    value = static_cast<std::uint16_t>(parsed);
    return true;
}

bool parseMac(const char* text, Load::MacAddress& mac)
{
    if (text == nullptr) return false;

    unsigned int byte[6]{};
    char trailing = '\0';

    const int count = std::sscanf(
        text,
        "%2x:%2x:%2x:%2x:%2x:%2x%c",
        &byte[0], &byte[1], &byte[2],
        &byte[3], &byte[4], &byte[5],
        &trailing);

    if (count != 6) return false;

    for (std::size_t i = 0U; i < 6U; ++i) {
        if (byte[i] > 255U) return false;
        mac[i] = static_cast<std::uint8_t>(byte[i]);
    }

    return true;
}

bool parseMode(const char* text, LoadMode::Value& mode)
{
    if (same(text, "FIXED_OFF")) {
        mode = LoadMode::Value::FIXED_OFF;
    } else if (same(text, "FIXED_ON")) {
        mode = LoadMode::Value::FIXED_ON;
    } else if (same(text, "AUTO_OFF")) {
        mode = LoadMode::Value::AUTO_OFF;
    } else if (same(text, "AUTO_ON")) {
        mode = LoadMode::Value::AUTO_ON;
    } else {
        return false;
    }

    return true;
}

bool parsePowerType(const char* text, LoadPowerType& powerType)
{
    if (same(text, "AC") || same(text, "ac")) {
        powerType = LoadPowerType::AC;
        return true;
    }
    if (same(text, "DC") || same(text, "dc")) {
        powerType = LoadPowerType::DC;
        return true;
    }
    return false;
}

bool parseSchedule(const char* text, AutoSchedule& schedule)
{
    if (text == nullptr) return false;

    if (same(text, "none") || same(text, "off")) {
        schedule = AutoSchedule{};
        return true;
    }

    unsigned int startHour = 0U;
    unsigned int startMinute = 0U;
    unsigned int endHour = 0U;
    unsigned int endMinute = 0U;
    char trailing = '\0';

    if (std::sscanf(
            text,
            "%u:%u-%u:%u%c",
            &startHour,
            &startMinute,
            &endHour,
            &endMinute,
            &trailing) != 4 ||
        startHour > 23U || startMinute > 59U ||
        endHour > 23U || endMinute > 59U) {
        return false;
    }

    const unsigned int startTotalMinutes =
        startHour * 60U + startMinute;
    const unsigned int endTotalMinutes =
        endHour * 60U + endMinute;

    if (startTotalMinutes == endTotalMinutes) return false;

    schedule = AutoSchedule{
        true,
        static_cast<std::uint8_t>(startHour),
        static_cast<std::uint8_t>(startMinute),
        static_cast<std::uint8_t>(endHour),
        static_cast<std::uint8_t>(endMinute)};
    return true;
}

std::uint32_t allocateConsoleCommandId()
{
    static std::uint32_t next = 1U;
    if (next == 0U) next = 1U;
    return next++;
}

int showResult(const CommandResult& result)
{
    if (!result.accepted) {
        std::printf(
            "[FAIL] %s\n",
            result.reason[0] != '\0'
                ? result.reason
                : "command rejected");
        return 1;
    }

    std::printf(
        "%s %s\n",
        result.completed ? "[ OK ]" : "[INFO]",
        result.reason[0] != '\0'
            ? result.reason
            : (result.completed
                   ? "command completed"
                   : "command accepted"));
    return 0;
}

void printBanner()
{
    std::printf("\n");
    std::printf("============================================================\n");
    std::printf("                         KILOWATTS\n");
    std::printf("                  Central Energy Controller\n");
    std::printf("============================================================\n");
    std::printf("Type 'help' for the ESP console command list.\n");
    std::printf("Use '<command> help' for command-specific usage.\n\n");
}

void statusUsage() { std::printf("Usage: status\n"); }
void dashboardUsage() { std::printf("Usage: dashboard\n"); }
void sensorUsage() { std::printf("Usage: sensor sim|ina219\n"); }
void nodesUsage() { std::printf("Usage: nodes\n"); }
void loadsUsage() { std::printf("Usage: loads\n"); }

void batteryUsage()
{
    std::printf(
        "Usage:\n"
        "  battery\n"
        "  battery status\n"
        "  battery configure shunt_ohms=R max_sensor_amps=A ema_alpha=X "
        "capacity_ah=AH initial_soc=PERCENT nominal_voltage=V\n"
        "  battery planning budget=W reserve=W min_soc=PERCENT "
        "[runtime_hours=H]\n\n"
        "P_budget is the configured installation power allocation.\n"
        "P_reserve is power intentionally kept unused.\n"
        "INA219 and simulation supply voltage/current for P_measured.\n"
        "runtime_hours uses battery energy above min_soc to constrain "
        "AUTO allocation.\n");
}

void nodeUsage()
{
    std::printf(
        "Usage:\n"
        "  node show MAC\n"
        "  node commission MAC name=NAME\n"
        "  node rename MAC name=NAME\n"
        "  node decommission MAC\n");
}

void loadUsage()
{
    std::printf(
        "Usage:\n"
        "  load show PIN\n"
        "  load show MAC PIN\n"
        "  load add [mac=MAC] pin=PIN name=NAME power=W priority=N "
        "type=AC|DC active_high=on|off "
        "mode=FIXED_OFF|FIXED_ON|AUTO_OFF|AUTO_ON "
        "schedule=HH:MM-HH:MM|none\n"
        "  load remove MAC PIN\n"
        "  load set MAC PIN [priority=N] "
        "[mode=FIXED_OFF|FIXED_ON|AUTO_OFF|AUTO_ON] "
        "[schedule=HH:MM-HH:MM|none]\n");
}

void optimizeUsage()
{
    std::printf(
        "Usage:\n"
        "  optimize\n"
        "  optimize run\n"
        "  optimize status\n"
        "  optimize interval seconds=N\n"
        "  optimize interval minutes=N\n");
}

void wifiUsage()
{
    std::printf(
        "Usage:\n"
        "  wifi status\n"
        "  wifi scan\n"
        "  wifi setup\n"
        "  wifi set ssid=NAME password=PASSWORD\n"
        "  wifi channel CHANNEL\n"
        "  wifi clear\n");
}

void mqttUsage()
{
    std::printf(
        "Usage:\n"
        "  mqtt status\n"
        "  mqtt set host=HOST port=PORT tls=on|off "
        "[username=USER] [password=PASSWORD]\n"
        "  mqtt clear\n");
}

void simulationUsage()
{
    std::printf(
        "Usage:\n"
        "  simulation start\n"
        "  simulation stop\n"
        "  simulation values [voltage=V current=A] [soc=PERCENT]\n"
        "Provide voltage and current together. "
        "At least one value group is required.\n");
}

void systemUsage()
{
    std::printf(
        "Usage:\n"
        "  system reset\n"
        "  system factory-reset confirm=RESET\n"
        "  system factory-reset mac=AA:BB:CC:DD:EE:FF confirm=RESET\n");
}

NetworkCommandRequest makeNetworkRequest(
    NetworkCommandTarget target,
    NetworkCommandRequest::Action action)
{
    NetworkCommandRequest request{};
    request.target = target;
    request.action = action;
    return request;
}

} // namespace

CentralConsole* CentralConsole::active_ = nullptr;

bool CentralConsole::begin(const Callbacks& callbacks)
{
    callbacks_ = callbacks;
    active_ = this;

    const esp_console_cmd_t commands[] = {
        {"status", "Quick Central status", nullptr, &CentralConsole::status, nullptr, nullptr, nullptr},
        {"dashboard", "Complete Kilowatts dashboard", nullptr, &CentralConsole::dashboard, nullptr, nullptr, nullptr},
        {"battery", "Battery measurement and power planning", nullptr, &CentralConsole::battery, nullptr, nullptr, nullptr},
        {"bat", "Alias for battery", nullptr, &CentralConsole::battery, nullptr, nullptr, nullptr},
        {"sensor", "Select simulated input or physical INA219", nullptr, &CentralConsole::sensor, nullptr, nullptr, nullptr},
        {"nodes", "List registered Nodes", nullptr, &CentralConsole::nodes, nullptr, nullptr, nullptr},
        {"node", "Show, commission, rename or decommission a Node", nullptr, &CentralConsole::node, nullptr, nullptr, nullptr},
        {"loads", "List configured Loads", nullptr, &CentralConsole::loads, nullptr, nullptr, nullptr},
        {"load", "Show, add, remove or update a Load", nullptr, &CentralConsole::load, nullptr, nullptr, nullptr},
        {"optimize", "Run one Best-First Search control cycle", nullptr, &CentralConsole::optimize, nullptr, nullptr, nullptr},
        {"wifi", "Wi-Fi status and provisioning", nullptr, &CentralConsole::wifi, nullptr, nullptr, nullptr},
        {"mqtt", "MQTT status and configuration", nullptr, &CentralConsole::mqtt, nullptr, nullptr, nullptr},
        {"simulation", "Control the battery-input simulator", nullptr, &CentralConsole::simulation, nullptr, nullptr, nullptr},
        {"sim", "Alias for simulation", nullptr, &CentralConsole::simulation, nullptr, nullptr, nullptr},
        {"system", "Reboot or factory-reset", nullptr, &CentralConsole::system, nullptr, nullptr, nullptr},
        {"clear", "Clear the terminal", nullptr, &CentralConsole::clear, nullptr, nullptr, nullptr},
    };

    for (const auto& command : commands) {
        const esp_err_t result = esp_console_cmd_register(&command);
        if (result != ESP_OK) {
            ESP_LOGE(
                TAG,
                "command registration failed (%s): %s",
                command.command,
                esp_err_to_name(result));
            return false;
        }
    }

    esp_console_repl_config_t replConfiguration =
        ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    replConfiguration.prompt = "kilowatts > ";
    replConfiguration.max_cmdline_length = 384U;

    esp_console_dev_uart_config_t uartConfiguration =
        ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_console_repl_t* repl = nullptr;

    esp_err_t result = esp_console_new_repl_uart(
        &uartConfiguration,
        &replConfiguration,
        &repl);

    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "UART REPL creation failed: %s",
            esp_err_to_name(result));
        return false;
    }

    printBanner();

    result = esp_console_start_repl(repl);
    if (result != ESP_OK) {
        ESP_LOGE(
            TAG,
            "UART REPL start failed: %s",
            esp_err_to_name(result));
        return false;
    }

    return true;
}

int CentralConsole::status(int argc, char** argv)
{
    if (helpRequested(argc, argv)) {
        statusUsage();
        return 0;
    }

    if (argc != 1 ||
        active_ == nullptr ||
        active_->callbacks_.status == nullptr) {
        statusUsage();
        return 1;
    }

    active_->callbacks_.status(active_->callbacks_.context);
    return 0;
}

int CentralConsole::dashboard(int argc, char** argv)
{
    if (helpRequested(argc, argv)) {
        dashboardUsage();
        return 0;
    }

    if (argc != 1 ||
        active_ == nullptr ||
        active_->callbacks_.dashboard == nullptr) {
        dashboardUsage();
        return 1;
    }

    active_->callbacks_.dashboard(active_->callbacks_.context);
    return 0;
}

int CentralConsole::battery(int argc, char** argv)
{
    if (active_ == nullptr) return 1;

    if (helpRequested(argc, argv)) {
        batteryUsage();
        return 0;
    }

    if (argc == 1 ||
        (argc == 2 && same(argv[1], "status"))) {
        if (active_->callbacks_.batteryStatus == nullptr) {
            std::printf("[FAIL] battery status is unavailable\n");
            return 1;
        }

        active_->callbacks_.batteryStatus(active_->callbacks_.context);
        return 0;
    }

    if (argc >= 2 && same(argv[1], "planning")) {
        if (active_->callbacks_.configurePowerPlanning == nullptr) {
            std::printf(
                "[FAIL] power planning configuration is unavailable\n");
            return 1;
        }

        PowerPlanningCommandRequest request{};

        if (!parseFloat(
                option(argc, argv, "budget"),
                request.P_budget) ||
            !parseFloat(
                option(argc, argv, "reserve"),
                request.P_reserve) ||
            !parseFloat(
                option(argc, argv, "min_soc"),
                request.minimumStateOfChargePercent)) {
            batteryUsage();
            return 1;
        }

        const char* runtimeText =
            option(argc, argv, "runtime_hours");
        request.requiredRuntimeHours = 0.0F;

        if (runtimeText != nullptr &&
            !parseFloat(
                runtimeText,
                request.requiredRuntimeHours)) {
            batteryUsage();
            return 1;
        }

        if (request.P_budget <= 0.0F ||
            request.P_reserve < 0.0F ||
            request.P_reserve > request.P_budget ||
            request.minimumStateOfChargePercent < 0.0F ||
            request.minimumStateOfChargePercent > 100.0F ||
            request.requiredRuntimeHours < 0.0F) {
            std::printf(
                "[FAIL] invalid power planning configuration\n");
            return 1;
        }

        return showResult(
            active_->callbacks_.configurePowerPlanning(
                active_->callbacks_.context,
                request));
    }

    if (argc < 2 ||
        !same(argv[1], "configure") ||
        active_->callbacks_.configureBattery == nullptr) {
        batteryUsage();
        return 1;
    }

    BatterySensorCommandRequest request{};

    if (!parseFloat(
            option(argc, argv, "shunt_ohms"),
            request.shuntResistanceOhms) ||
        !parseFloat(
            option(argc, argv, "max_sensor_amps"),
            request.maximumExpectedCurrentAmps) ||
        !parseFloat(
            option(argc, argv, "ema_alpha"),
            request.emaAlpha) ||
        !parseFloat(
            option(argc, argv, "capacity_ah"),
            request.batteryCapacityAmpHours) ||
        !parseFloat(
            option(argc, argv, "initial_soc"),
            request.initialStateOfChargePercent) ||
        !parseFloat(
            option(argc, argv, "nominal_voltage"),
            request.nominalVoltageVolts)) {
        batteryUsage();
        return 1;
    }

    if (request.shuntResistanceOhms <= 0.0F ||
        request.maximumExpectedCurrentAmps <= 0.0F ||
        request.emaAlpha <= 0.0F ||
        request.emaAlpha > 1.0F ||
        request.batteryCapacityAmpHours <= 0.0F ||
        request.initialStateOfChargePercent < 0.0F ||
        request.initialStateOfChargePercent > 100.0F ||
        request.nominalVoltageVolts <= 0.0F) {
        std::printf("[FAIL] invalid battery-monitor configuration\n");
        return 1;
    }

    return showResult(
        active_->callbacks_.configureBattery(
            active_->callbacks_.context,
            request));
}

int CentralConsole::sensor(int argc, char** argv)
{
    if (helpRequested(argc, argv)) {
        sensorUsage();
        return 0;
    }

    if (argc != 2 ||
        active_ == nullptr ||
        active_->callbacks_.sensorMode == nullptr) {
        sensorUsage();
        return 1;
    }

    const bool simulated = same(argv[1], "sim");
    const bool hardware = same(argv[1], "ina219");

    if (!simulated && !hardware) {
        sensorUsage();
        return 1;
    }

    const bool changed = active_->callbacks_.sensorMode(
        active_->callbacks_.context,
        simulated);

    std::printf(
        "%s Battery measurement source: %s\n",
        changed ? "[ OK ]" : "[FAIL]",
        changed
            ? (simulated ? "SIMULATION" : "INA219")
            : "CHANGE FAILED");

    return changed ? 0 : 1;
}

int CentralConsole::nodes(int argc, char** argv)
{
    if (helpRequested(argc, argv)) {
        nodesUsage();
        return 0;
    }

    if (argc != 1 ||
        active_ == nullptr ||
        active_->callbacks_.nodes == nullptr) {
        nodesUsage();
        return 1;
    }

    active_->callbacks_.nodes(active_->callbacks_.context);
    return 0;
}

int CentralConsole::node(int argc, char** argv)
{
    if (active_ == nullptr) return 1;

    if (helpRequested(argc, argv) || argc < 2) {
        nodeUsage();
        return helpRequested(argc, argv) ? 0 : 1;
    }

    if (same(argv[1], "show")) {
        if (argc != 3 || active_->callbacks_.nodeStatus == nullptr) {
            nodeUsage();
            return 1;
        }

        Load::MacAddress mac{};
        if (!parseMac(argv[2], mac)) {
            std::printf("[FAIL] invalid MAC address\n");
            return 1;
        }

        active_->callbacks_.nodeStatus(
            active_->callbacks_.context,
            mac);
        return 0;
    }

    if (same(argv[1], "decommission")) {
        if (argc != 3 || active_->callbacks_.nodeCommand == nullptr) {
            nodeUsage();
            return 1;
        }

        NodeCommandRequest request{};
        request.action = NodeCommandRequest::Action::DECOMMISSION;

        if (!parseMac(argv[2], request.nodeMacAddress)) {
            std::printf("[FAIL] invalid MAC address\n");
            return 1;
        }

        return showResult(
            active_->callbacks_.nodeCommand(
                active_->callbacks_.context,
                request));
    }

    if ((!same(argv[1], "commission") &&
         !same(argv[1], "rename")) ||
        argc < 3 ||
        active_->callbacks_.nodeCommand == nullptr) {
        nodeUsage();
        return 1;
    }

    NodeCommandRequest request{};
    request.action = same(argv[1], "commission")
        ? NodeCommandRequest::Action::COMMISSION
        : NodeCommandRequest::Action::RENAME;

    if (!parseMac(argv[2], request.nodeMacAddress) ||
        !copyText(
            request.friendlyName,
            sizeof(request.friendlyName),
            option(argc, argv, "name"))) {
        nodeUsage();
        return 1;
    }

    return showResult(
        active_->callbacks_.nodeCommand(
            active_->callbacks_.context,
            request));
}

int CentralConsole::loads(int argc, char** argv)
{
    if (helpRequested(argc, argv)) {
        loadsUsage();
        return 0;
    }

    if (argc != 1 ||
        active_ == nullptr ||
        active_->callbacks_.loads == nullptr) {
        loadsUsage();
        return 1;
    }

    active_->callbacks_.loads(active_->callbacks_.context);
    return 0;
}

int CentralConsole::load(int argc, char** argv)
{
    if (active_ == nullptr) return 1;

    if (helpRequested(argc, argv) || argc < 2) {
        loadUsage();
        return helpRequested(argc, argv) ? 0 : 1;
    }

    if (same(argv[1], "show")) {
        if (active_->callbacks_.loadStatus == nullptr) {
            std::printf("[FAIL] load status is unavailable\n");
            return 1;
        }

        Load::MacAddress mac{};
        std::uint8_t relayPin = 0U;

        if (argc == 3) {
            if (active_->callbacks_.localMac == nullptr ||
                !parseUint8(argv[2], relayPin)) {
                loadUsage();
                return 1;
            }
            mac = active_->callbacks_.localMac(
                active_->callbacks_.context);
        } else if (argc == 4) {
            if (!parseMac(argv[2], mac) ||
                !parseUint8(argv[3], relayPin)) {
                loadUsage();
                return 1;
            }
        } else {
            loadUsage();
            return 1;
        }

        active_->callbacks_.loadStatus(
            active_->callbacks_.context,
            mac,
            relayPin);
        return 0;
    }

    if (same(argv[1], "remove")) {
        if (argc != 4 || active_->callbacks_.removeLoad == nullptr) {
            loadUsage();
            return 1;
        }

        RemoveLoadCommandRequest request{};
        if (!parseMac(argv[2], request.nodeMacAddress) ||
            !parseUint8(argv[3], request.relayPin)) {
            std::printf("[FAIL] invalid MAC address or relay pin\n");
            return 1;
        }

        return showResult(
            active_->callbacks_.removeLoad(
                active_->callbacks_.context,
                request));
    }

    if (same(argv[1], "set")) {
        if (argc < 4 || active_->callbacks_.loadCommand == nullptr) {
            loadUsage();
            return 1;
        }

        LoadCommandRequest request{};
        request.commandId = allocateConsoleCommandId();

        if (!parseMac(argv[2], request.nodeMacAddress) ||
            !parseUint8(argv[3], request.relayPin)) {
            std::printf("[FAIL] invalid MAC address or relay pin\n");
            return 1;
        }

        const char* priorityText = option(argc, argv, "priority");
        if (priorityText != nullptr) {
            if (!parseUint16(priorityText, request.priority)) {
                loadUsage();
                return 1;
            }
            request.hasPriority = true;
        }

        const char* modeText = option(argc, argv, "mode");
        if (modeText != nullptr) {
            if (!parseMode(modeText, request.mode)) {
                loadUsage();
                return 1;
            }
            request.hasMode = true;
        }

        const char* scheduleText = option(argc, argv, "schedule");
        if (scheduleText != nullptr) {
            if (!parseSchedule(scheduleText, request.schedule)) {
                loadUsage();
                return 1;
            }
            request.hasSchedule = true;
        }

        if (!request.hasPriority &&
            !request.hasMode &&
            !request.hasSchedule) {
            std::printf(
                "[FAIL] provide at least one of priority=, mode=, "
                "schedule=\n");
            return 1;
        }

        return showResult(
            active_->callbacks_.loadCommand(
                active_->callbacks_.context,
                request));
    }

    if (!same(argv[1], "add") ||
        active_->callbacks_.configureLoad == nullptr) {
        loadUsage();
        return 1;
    }

    LoadConfigurationCommandRequest request{};

    const char* macText = option(argc, argv, "mac");
    if (macText != nullptr) {
        if (!parseMac(macText, request.nodeMacAddress)) {
            std::printf("[FAIL] invalid Node MAC address\n");
            return 1;
        }
    } else {
        if (active_->callbacks_.localMac == nullptr) {
            std::printf(
                "[FAIL] Central MAC unavailable; provide mac=MAC\n");
            return 1;
        }
        request.nodeMacAddress = active_->callbacks_.localMac(
            active_->callbacks_.context);
    }

    if (!parseUint8(
            option(argc, argv, "pin"),
            request.relayPin) ||
        !copyText(
            request.name,
            sizeof(request.name),
            option(argc, argv, "name")) ||
        !parseFloat(
            option(argc, argv, "power"),
            request.powerRatingWatts) ||
        !parseUint16(
            option(argc, argv, "priority"),
            request.priority) ||
        !parsePowerType(
            option(argc, argv, "type"),
            request.powerType) ||
        !parseMode(
            option(argc, argv, "mode"),
            request.mode) ||
        !parseSchedule(
            option(argc, argv, "schedule"),
            request.schedule)) {
        loadUsage();
        return 1;
    }

    const char* activeHighText =
        option(argc, argv, "active_high");
    if (activeHighText == nullptr ||
        (!same(activeHighText, "on") &&
         !same(activeHighText, "off"))) {
        loadUsage();
        return 1;
    }

    request.relayActiveHigh = same(activeHighText, "on");

    if (request.powerRatingWatts < 0.0F) {
        std::printf("[FAIL] power rating cannot be negative\n");
        return 1;
    }

    return showResult(
        active_->callbacks_.configureLoad(
            active_->callbacks_.context,
            request));
}

int CentralConsole::optimize(int argc, char** argv)
{
    if (helpRequested(argc, argv)) {
        optimizeUsage();
        return 0;
    }

    if (active_ == nullptr) return 1;

    if (argc == 1 ||
        (argc == 2 && same(argv[1], "run"))) {
        if (active_->callbacks_.optimize == nullptr) return 1;
        active_->callbacks_.optimize(active_->callbacks_.context);
        return 0;
    }

    if (argc == 2 && same(argv[1], "status")) {
        if (active_->callbacks_.system == nullptr) return 1;

        SystemCommandRequest request{};
        request.commandId = allocateConsoleCommandId();
        request.action =
            SystemCommandAction::REPORT_OPTIMIZER_INTERVAL;

        return showResult(
            active_->callbacks_.system(
                active_->callbacks_.context,
                request));
    }

    if (argc == 3 && same(argv[1], "interval")) {
        if (active_->callbacks_.system == nullptr) return 1;

        unsigned long parsed = 0U;
        const char* secondsText = option(argc, argv, "seconds");
        const char* minutesText = option(argc, argv, "minutes");

        if (secondsText != nullptr) {
            if (!parseUnsigned(secondsText, parsed) ||
                parsed == 0UL ||
                parsed > 86400UL) {
                optimizeUsage();
                return 1;
            }
        } else if (minutesText != nullptr) {
            if (!parseUnsigned(minutesText, parsed) ||
                parsed == 0UL ||
                parsed > 1440UL) {
                optimizeUsage();
                return 1;
            }
            parsed *= 60UL;
        } else {
            optimizeUsage();
            return 1;
        }

        SystemCommandRequest request{};
        request.commandId = allocateConsoleCommandId();
        request.action = SystemCommandAction::SET_OPTIMIZER_INTERVAL;
        request.hasOptimizerIntervalSeconds = true;
        request.optimizerIntervalSeconds =
            static_cast<std::uint32_t>(parsed);

        return showResult(
            active_->callbacks_.system(
                active_->callbacks_.context,
                request));
    }

    optimizeUsage();
    return 1;
}

int CentralConsole::wifi(int argc, char** argv)
{
    if (active_ == nullptr ||
        active_->callbacks_.network == nullptr) {
        return 1;
    }

    if (helpRequested(argc, argv) || argc < 2) {
        wifiUsage();
        return helpRequested(argc, argv) ? 0 : 1;
    }

    NetworkCommandRequest request{};

    if (same(argv[1], "status")) {
        request = makeNetworkRequest(
            NetworkCommandTarget::WIFI,
            NetworkCommandRequest::Action::STATUS);
    } else if (same(argv[1], "scan")) {
        request = makeNetworkRequest(
            NetworkCommandTarget::WIFI,
            NetworkCommandRequest::Action::SCAN);
    } else if (same(argv[1], "setup")) {
        request = makeNetworkRequest(
            NetworkCommandTarget::WIFI,
            NetworkCommandRequest::Action::SETUP);
    } else if (same(argv[1], "clear")) {
        request = makeNetworkRequest(
            NetworkCommandTarget::WIFI,
            NetworkCommandRequest::Action::CLEAR);
    } else if (same(argv[1], "channel")) {
        request = makeNetworkRequest(
            NetworkCommandTarget::WIFI,
            NetworkCommandRequest::Action::SET_CHANNEL);

        if (argc != 3 ||
            !parseUint8(argv[2], request.wifiChannel)) {
            wifiUsage();
            return 1;
        }
    } else if (same(argv[1], "set")) {
        request = makeNetworkRequest(
            NetworkCommandTarget::WIFI,
            NetworkCommandRequest::Action::SET);

        if (!copyText(
                request.ssid,
                sizeof(request.ssid),
                option(argc, argv, "ssid")) ||
            !copyText(
                request.wifiPassword,
                sizeof(request.wifiPassword),
                option(argc, argv, "password"))) {
            wifiUsage();
            return 1;
        }
    } else {
        wifiUsage();
        return 1;
    }

    return showResult(
        active_->callbacks_.network(
            active_->callbacks_.context,
            request));
}

int CentralConsole::mqtt(int argc, char** argv)
{
    if (active_ == nullptr ||
        active_->callbacks_.network == nullptr) {
        return 1;
    }

    if (helpRequested(argc, argv) || argc < 2) {
        mqttUsage();
        return helpRequested(argc, argv) ? 0 : 1;
    }

    NetworkCommandRequest request{};

    if (same(argv[1], "status")) {
        request = makeNetworkRequest(
            NetworkCommandTarget::MQTT,
            NetworkCommandRequest::Action::STATUS);
    } else if (same(argv[1], "clear")) {
        request = makeNetworkRequest(
            NetworkCommandTarget::MQTT,
            NetworkCommandRequest::Action::CLEAR);
    } else if (same(argv[1], "set")) {
        request = makeNetworkRequest(
            NetworkCommandTarget::MQTT,
            NetworkCommandRequest::Action::SET);

        unsigned long port = 0U;
        const char* tls = option(argc, argv, "tls");

        if (!copyText(
                request.mqttHost,
                sizeof(request.mqttHost),
                option(argc, argv, "host")) ||
            !parseUnsigned(
                option(argc, argv, "port"),
                port) ||
            port == 0U || port > 65535U ||
            (tls == nullptr ||
             (!same(tls, "on") && !same(tls, "off")))) {
            mqttUsage();
            return 1;
        }

        request.mqttPort = static_cast<std::uint16_t>(port);
        request.mqttUseTls = same(tls, "on");

        const char* username = option(argc, argv, "username");
        const char* password = option(argc, argv, "password");

        if (username != nullptr &&
            !copyText(
                request.mqttUsername,
                sizeof(request.mqttUsername),
                username)) {
            std::printf("[FAIL] MQTT username is too long\n");
            return 1;
        }

        if (password != nullptr &&
            !copyText(
                request.mqttPassword,
                sizeof(request.mqttPassword),
                password)) {
            std::printf("[FAIL] MQTT password is too long\n");
            return 1;
        }
    } else {
        mqttUsage();
        return 1;
    }

    return showResult(
        active_->callbacks_.network(
            active_->callbacks_.context,
            request));
}

int CentralConsole::simulation(int argc, char** argv)
{
    if (active_ == nullptr ||
        active_->callbacks_.simulation == nullptr) {
        return 1;
    }

    if (helpRequested(argc, argv) || argc < 2) {
        simulationUsage();
        return helpRequested(argc, argv) ? 0 : 1;
    }

    SimulationCommandRequest request{};
    request.commandId = allocateConsoleCommandId();

    if (same(argv[1], "start") && argc == 2) {
        request.action = SimulationCommandAction::ENABLE;
    } else if (same(argv[1], "stop") && argc == 2) {
        request.action = SimulationCommandAction::DISABLE;
    } else if (same(argv[1], "values")) {
        request.action = SimulationCommandAction::SET_VALUES;

        const char* voltageText = option(argc, argv, "voltage");
        const char* currentText = option(argc, argv, "current");
        const char* socText = option(argc, argv, "soc");

        if ((voltageText == nullptr) != (currentText == nullptr)) {
            simulationUsage();
            return 1;
        }

        if (voltageText != nullptr) {
            if (!parseFloat(
                    voltageText,
                    request.batteryVoltageVolts) ||
                !parseFloat(
                    currentText,
                    request.batteryCurrentAmps)) {
                simulationUsage();
                return 1;
            }
            request.hasElectricalMeasurements = true;
        }

        if (socText != nullptr) {
            if (!parseFloat(
                    socText,
                    request.stateOfChargePercent) ||
                request.stateOfChargePercent < 0.0F ||
                request.stateOfChargePercent > 100.0F) {
                simulationUsage();
                return 1;
            }
            request.hasStateOfChargePercent = true;
        }

        if (!request.hasElectricalMeasurements &&
            !request.hasStateOfChargePercent) {
            simulationUsage();
            return 1;
        }
    } else {
        simulationUsage();
        return 1;
    }

    return showResult(
        active_->callbacks_.simulation(
            active_->callbacks_.context,
            request));
}

int CentralConsole::system(int argc, char** argv)
{
    if (active_ == nullptr ||
        active_->callbacks_.system == nullptr) {
        return 1;
    }

    if (helpRequested(argc, argv) || argc < 2) {
        systemUsage();
        return helpRequested(argc, argv) ? 0 : 1;
    }

    if (same(argv[1], "reset")) {
        if (option(argc, argv, "mac") != nullptr) {
            std::printf(
                "[FAIL] system reset reboots Central only; use system "
                "factory-reset mac=... for a Smart Node\n");
            return 1;
        }

        SystemCommandRequest request{};
        request.commandId = allocateConsoleCommandId();
        request.action = SystemCommandAction::REBOOT_CENTRAL;
        request.hasTargetNodeMacAddress = false;

        return showResult(
            active_->callbacks_.system(
                active_->callbacks_.context,
                request));
    }

    if (!same(argv[1], "factory-reset")) {
        systemUsage();
        return 1;
    }

    const char* confirmation = option(argc, argv, "confirm");
    if (!same(confirmation, "RESET")) {
        std::printf(
            "[FAIL] factory-reset requires confirm=RESET\n");
        return 1;
    }

    SystemCommandRequest request{};
    request.commandId = allocateConsoleCommandId();
    copyText(
        request.confirmText,
        sizeof(request.confirmText),
        confirmation);

    const char* macText = option(argc, argv, "mac");
    if (macText == nullptr) {
        request.action = SystemCommandAction::FACTORY_RESET_CENTRAL;
        request.hasTargetNodeMacAddress = false;
    } else {
        request.action = SystemCommandAction::FACTORY_RESET_NODE;
        request.hasTargetNodeMacAddress = true;

        if (!parseMac(macText, request.targetNodeMacAddress)) {
            std::printf("[FAIL] invalid target Node MAC address\n");
            return 1;
        }
    }

    return showResult(
        active_->callbacks_.system(
            active_->callbacks_.context,
            request));
}

int CentralConsole::clear(int argc, char** argv)
{
    (void)argv;

    if (argc != 1) {
        std::printf("Usage: clear\n");
        return 1;
    }

    std::printf("\033[2J\033[H");
    std::fflush(stdout);
    return 0;
}

} // namespace kilowatts
