#include "SmartConsole.h"

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

constexpr const char* TAG = "SMART_CONSOLE";

bool same(const char* first, const char* second)
{
    return first != nullptr && second != nullptr && std::strcmp(first, second) == 0;
}

bool helpRequested(int argc, char** argv)
{
    return argc == 2 &&
           (same(argv[1], "help") || same(argv[1], "--help") || same(argv[1], "-h"));
}

const char* option(int argc, char** argv, const char* name)
{
    if (name == nullptr) return nullptr;

    const std::size_t length = std::strlen(name);
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == nullptr) continue;
        if (std::strncmp(argv[i], name, length) == 0 && argv[i][length] == '=') {
            return argv[i] + length + 1U;
        }
    }
    return nullptr;
}

bool copyText(char* destination, std::size_t destinationSize, const char* source)
{
    if (destination == nullptr || destinationSize == 0U || source == nullptr) return false;
    if (std::strlen(source) >= destinationSize) return false;
    std::snprintf(destination, destinationSize, "%s", source);
    return true;
}

bool parseFloat(const char* text, float& value)
{
    if (text == nullptr || *text == '\0') return false;
    char* end = nullptr;
    const float parsed = std::strtof(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(parsed)) return false;
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
    if (!parseUnsigned(text, parsed) || parsed > std::numeric_limits<std::uint8_t>::max()) {
        return false;
    }
    value = static_cast<std::uint8_t>(parsed);
    return true;
}

bool parseUint16(const char* text, std::uint16_t& value)
{
    unsigned long parsed = 0U;
    if (!parseUnsigned(text, parsed) || parsed > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    value = static_cast<std::uint16_t>(parsed);
    return true;
}

bool parseMode(const char* text, LoadMode::Value& mode)
{
    if (same(text, "FIXED_OFF")) mode = LoadMode::Value::FIXED_OFF;
    else if (same(text, "FIXED_ON")) mode = LoadMode::Value::FIXED_ON;
    else if (same(text, "AUTO_OFF")) mode = LoadMode::Value::AUTO_OFF;
    else if (same(text, "AUTO_ON")) mode = LoadMode::Value::AUTO_ON;
    else return false;
    return true;
}

bool parsePowerType(const char* text, LoadPowerType& powerType)
{
    if (same(text, "AC") || same(text, "ac")) { powerType = LoadPowerType::AC; return true; }
    if (same(text, "DC") || same(text, "dc")) { powerType = LoadPowerType::DC; return true; }
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
            &startHour, &startMinute, &endHour, &endMinute, &trailing) != 4 ||
        startHour > 23U || startMinute > 59U ||
        endHour > 23U || endMinute > 59U) {
        return false;
    }

    const unsigned int startTotalMinutes = startHour * 60U + startMinute;
    const unsigned int endTotalMinutes = endHour * 60U + endMinute;
    if (startTotalMinutes == endTotalMinutes) return false;

    schedule = AutoSchedule{
        true,
        static_cast<std::uint8_t>(startHour),
        static_cast<std::uint8_t>(startMinute),
        static_cast<std::uint8_t>(endHour),
        static_cast<std::uint8_t>(endMinute)};
    return true;
}

int showResult(const CommandResult& result)
{
    if (!result.accepted) {
        std::printf("[FAIL] %s\n", result.reason[0] != '\0' ? result.reason : "command rejected");
        return 1;
    }

    std::printf("%s %s\n",
                result.completed ? "[ OK ]" : "[INFO]",
                result.reason[0] != '\0' ? result.reason :
                (result.completed ? "command completed" : "command accepted"));
    return 0;
}

void loadUsage()
{
    std::printf(
        "Usage:\n"
        "  load show <pin>\n"
        "  load add pin=N name=TEXT power=W priority=0..10 type=AC|DC active_high=on|off mode=FIXED_OFF|FIXED_ON|AUTO_OFF|AUTO_ON schedule=HH:MM-HH:MM|none\n"
        "  load remove <pin>\n"
        "  load set <pin> [priority=0..10] [mode=...] [schedule=...]\n");
}

void printBanner()
{
    std::printf(
        "\n"
        "============================================================\n"
        "                    KILOWATTS SMART NODE\n"
        "============================================================\n"
        "Type 'help' for the console command list.\n"
        "Minimal console: status, loads, load show/add/remove/set.\n"
        "Loads changed here are this node's own - Central will see them\n"
        "on its next report cycle.\n\n");
}

} // namespace

SmartConsole* SmartConsole::active_ = nullptr;

int SmartConsole::status(int argc, char** argv)
{
    (void)argv;
    if (argc != 1 || active_ == nullptr || active_->callbacks_.status == nullptr) return 1;
    active_->callbacks_.status(active_->callbacks_.context);
    return 0;
}

int SmartConsole::loads(int argc, char** argv)
{
    (void)argv;
    if (argc != 1 || active_ == nullptr || active_->callbacks_.loads == nullptr) return 1;
    active_->callbacks_.loads(active_->callbacks_.context);
    return 0;
}

int SmartConsole::load(int argc, char** argv)
{
    if (active_ == nullptr) return 1;
    if (helpRequested(argc, argv) || argc < 2) {
        loadUsage();
        return helpRequested(argc, argv) ? 0 : 1;
    }

    if (same(argv[1], "show")) {
        std::uint8_t relayPin = 0U;
        if (argc != 3 || !parseUint8(argv[2], relayPin) ||
            active_->callbacks_.loadStatus == nullptr) {
            loadUsage();
            return 1;
        }
        active_->callbacks_.loadStatus(active_->callbacks_.context, relayPin);
        return 0;
    }

    if (same(argv[1], "remove")) {
        std::uint8_t relayPin = 0U;
        if (argc != 3 || !parseUint8(argv[2], relayPin) ||
            active_->callbacks_.removeLoad == nullptr) {
            loadUsage();
            return 1;
        }
        return showResult(active_->callbacks_.removeLoad(active_->callbacks_.context, relayPin));
    }

    if (same(argv[1], "set")) {
        std::uint8_t relayPin = 0U;
        if (argc < 3 || !parseUint8(argv[2], relayPin) ||
            active_->callbacks_.updateLoad == nullptr) {
            loadUsage();
            return 1;
        }

        SmartLoadUpdateRequest request{};
        request.relayPin = relayPin;

        const char* priorityText = option(argc, argv, "priority");
        if (priorityText != nullptr) {
            if (!parseUint16(priorityText, request.priority)) { loadUsage(); return 1; }
            request.hasPriority = true;
        }

        const char* modeText = option(argc, argv, "mode");
        if (modeText != nullptr) {
            if (!parseMode(modeText, request.mode)) { loadUsage(); return 1; }
            request.hasMode = true;
        }

        const char* scheduleText = option(argc, argv, "schedule");
        if (scheduleText != nullptr) {
            if (!parseSchedule(scheduleText, request.schedule)) { loadUsage(); return 1; }
            request.hasSchedule = true;
        }

        if (!request.hasPriority && !request.hasMode && !request.hasSchedule) {
            std::printf("[FAIL] provide at least one of priority=, mode=, schedule=\n");
            return 1;
        }

        return showResult(active_->callbacks_.updateLoad(active_->callbacks_.context, request));
    }

    if (!same(argv[1], "add") || active_->callbacks_.configureLoad == nullptr) {
        loadUsage();
        return 1;
    }

    SmartLoadConfigurationRequest request{};

    if (!parseUint8(option(argc, argv, "pin"), request.relayPin) ||
        !copyText(request.name, sizeof(request.name), option(argc, argv, "name")) ||
        !parseFloat(option(argc, argv, "power"), request.powerRatingWatts) ||
        !parseUint16(option(argc, argv, "priority"), request.priority) ||
        !parsePowerType(option(argc, argv, "type"), request.powerType) ||
        !parseMode(option(argc, argv, "mode"), request.mode) ||
        !parseSchedule(option(argc, argv, "schedule"), request.schedule)) {
        loadUsage();
        return 1;
    }

    const char* activeHighText = option(argc, argv, "active_high");
    if (activeHighText == nullptr ||
        (!same(activeHighText, "on") && !same(activeHighText, "off"))) {
        loadUsage();
        return 1;
    }
    request.relayActiveHigh = same(activeHighText, "on");

    if (request.powerRatingWatts < 0.0F) {
        std::printf("[FAIL] power rating cannot be negative\n");
        return 1;
    }

    return showResult(active_->callbacks_.configureLoad(active_->callbacks_.context, request));
}

int SmartConsole::clear(int argc, char** argv)
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

bool SmartConsole::begin(const Callbacks& callbacks)
{
    callbacks_ = callbacks;
    active_ = this;

    /*
     * esp_console_new_repl_uart() performs the common console initialization.
     * Do not call esp_console_init() separately before it.
     */
    const esp_console_cmd_t commands[] = {
        {"status", "Quick Smart Node status", nullptr, &SmartConsole::status, nullptr, nullptr, nullptr},
        {"loads", "List this node's configured Loads", nullptr, &SmartConsole::loads, nullptr, nullptr, nullptr},
        {"load", "Show, add, remove or update a Load", nullptr, &SmartConsole::load, nullptr, nullptr, nullptr},
        {"clear", "Clear the terminal", nullptr, &SmartConsole::clear, nullptr, nullptr, nullptr},
    };

    for (const auto& command : commands) {
        const esp_err_t result = esp_console_cmd_register(&command);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "command registration failed (%s): %s",
                     command.command, esp_err_to_name(result));
            return false;
        }
    }

    esp_console_repl_config_t replConfiguration = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    replConfiguration.prompt = "smart > ";
    replConfiguration.max_cmdline_length = 384U;

    esp_console_dev_uart_config_t uartConfiguration = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_console_repl_t* repl = nullptr;

    esp_err_t result = esp_console_new_repl_uart(
        &uartConfiguration, &replConfiguration, &repl);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "UART REPL creation failed: %s", esp_err_to_name(result));
        return false;
    }

    printBanner();

    result = esp_console_start_repl(repl);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "UART REPL start failed: %s", esp_err_to_name(result));
        return false;
    }

    return true;
}

} // namespace kilowatts
