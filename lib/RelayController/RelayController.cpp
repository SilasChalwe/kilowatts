/**
 * @file RelayController.cpp
 * @brief Implements local relay GPIO actuation for the Loads wired to THIS Node.
 *
 * This file is split by ESP_PLATFORM, the macro ESP-IDF defines for every
 * source file it builds, the same way INA219Monitor.cpp and
 * RelayController.h are:
 *
 * - Relay registration and commanded-state bookkeeping is plain,
 *   hardware-free C++ and is always compiled, so it can be exercised by a
 *   host-native test (see test/RelayController/) with no ESP32 involved.
 * - Real GPIO configuration/read/write is compiled only under
 *   ESP_PLATFORM, using ESP-IDF's driver/gpio.h. A host build's version of
 *   every hardware-touching method simply returns false without
 *   fabricating a GPIO level.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
 */

#include "RelayController.h"

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "esp_log.h"
#endif

namespace kilowatts {


#ifdef ESP_PLATFORM
static const char *TAG = "RELAY_CONTROLLER";
#endif


RelayController::RelayController()
    : relays_()
{
}


RelayController::~RelayController()
{
}


RelayController::RegisteredRelay* RelayController::findMutableRelay(std::uint8_t relayPin)
{
    for (RegisteredRelay& relay : relays_) {
        if (relay.configuration.relayPin == relayPin) {
            return &relay;
        }
    }

    return nullptr;
}


const RelayController::RegisteredRelay* RelayController::findRelay(std::uint8_t relayPin) const
{
    for (const RegisteredRelay& relay : relays_) {
        if (relay.configuration.relayPin == relayPin) {
            return &relay;
        }
    }

    return nullptr;
}


bool RelayController::addRelay(const RelayConfiguration& configuration)
{
    if (isRelayRegistered(configuration.relayPin)) {
#ifdef ESP_PLATFORM
        ESP_LOGE(TAG, "Rejected relay: pin %u is already registered",
                 static_cast<unsigned int>(configuration.relayPin));
#endif
        return false;
    }

    RegisteredRelay relay{};
    relay.configuration = configuration;
    relay.commandedOn = configuration.initialStateOn;
    relay.hardwareApplied = false;

    relays_.push_back(relay);

    const bool hardwareConfigured = configureGpio(relays_.back());
    relays_.back().hardwareApplied = hardwareConfigured;

#ifdef ESP_PLATFORM
    if (!hardwareConfigured) {
        ESP_LOGW(TAG, "Relay pin %u registered but GPIO configuration failed; commands will be tracked but not applied",
                 static_cast<unsigned int>(configuration.relayPin));
    } else {
        ESP_LOGI(TAG, "Relay pin %u registered: activeHigh=%s initialState=%s",
                 static_cast<unsigned int>(configuration.relayPin),
                 configuration.activeHigh ? "Yes" : "No",
                 configuration.initialStateOn ? "ON" : "OFF");
    }
#endif

    return true;
}


bool RelayController::removeRelay(std::uint8_t relayPin)
{
    for (std::size_t index = 0U; index < relays_.size(); ++index) {
        RegisteredRelay& relay = relays_[index];
        if (relay.configuration.relayPin != relayPin) {
            continue;
        }

#ifdef ESP_PLATFORM
        /*
         * A channel is only removed as part of rolling back a failed
         * provisioning operation, but still establish the documented safe
         * OFF level before releasing the pin.
         */
        if (relay.hardwareApplied) {
            if (!writeGpioLevel(relay, false)) {
                ESP_LOGE(TAG, "Could not drive relay pin %u OFF while rolling back configuration",
                         static_cast<unsigned int>(relayPin));
            }
            const esp_err_t resetResult = gpio_reset_pin(
                static_cast<gpio_num_t>(relay.configuration.relayPin));
            if (resetResult != ESP_OK) {
                ESP_LOGE(TAG, "gpio_reset_pin() failed for relay pin %u during rollback: %s",
                         static_cast<unsigned int>(relayPin), esp_err_to_name(resetResult));
            }
        }
#endif

        relays_.erase(relays_.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }

    return false;
}


std::size_t RelayController::getNumberOfRelays() const
{
    return relays_.size();
}


const RelayController::RelayConfiguration* RelayController::getRelay(std::size_t relayIndex) const
{
    if (relayIndex >= relays_.size()) {
        return nullptr;
    }

    return &relays_[relayIndex].configuration;
}


bool RelayController::isRelayRegistered(std::uint8_t relayPin) const
{
    return findRelay(relayPin) != nullptr;
}


bool RelayController::setRelayState(std::uint8_t relayPin, bool on)
{
    RegisteredRelay* relay = findMutableRelay(relayPin);

    if (relay == nullptr) {
#ifdef ESP_PLATFORM
        ESP_LOGW(TAG, "Cannot command relay pin %u: not registered", static_cast<unsigned int>(relayPin));
#endif
        return false;
    }

    /*
     * The commanded state is recorded regardless of whether the physical
     * write succeeds, so a caller comparing getCommandedState() against a
     * later readBackState() can detect a real hardware fault instead of
     * the two values silently disagreeing forever.
     */
    relay->commandedOn = on;

    const bool written = writeGpioLevel(*relay, on);

#ifdef ESP_PLATFORM
    if (written) {
        ESP_LOGI(TAG, "Relay pin %u commanded %s", static_cast<unsigned int>(relayPin), on ? "ON" : "OFF");
    } else {
        ESP_LOGE(TAG, "Relay pin %u GPIO write failed while commanding %s",
                 static_cast<unsigned int>(relayPin), on ? "ON" : "OFF");
    }
#endif

    return written;
}


bool RelayController::getCommandedState(std::uint8_t relayPin, bool& on) const
{
    const RegisteredRelay* relay = findRelay(relayPin);

    if (relay == nullptr) {
        return false;
    }

    on = relay->commandedOn;
    return true;
}


bool RelayController::readBackState(std::uint8_t relayPin, bool& on) const
{
    const RegisteredRelay* relay = findRelay(relayPin);

    if (relay == nullptr) {
        return false;
    }

    return readGpioLevel(*relay, on);
}


bool RelayController::isHardwareApplied(std::uint8_t relayPin) const
{
    const RegisteredRelay* relay = findRelay(relayPin);
    return relay != nullptr && relay->hardwareApplied;
}


void RelayController::printDiagnosticReport() const
{
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "RELAY CHANNELS");
    ESP_LOGI(TAG, "--------------------------------------------------------------");
    ESP_LOGI(TAG, "%-9s %-11s %-12s %-10s", "Pin", "ActiveHigh", "Hardware", "Commanded");

    for (const RegisteredRelay& relay : relays_) {
        bool readBack = false;
        const bool readOk = readGpioLevel(relay, readBack);

        ESP_LOGI(TAG, "%-9u %-11s %-12s %-10s%s",
                 static_cast<unsigned int>(relay.configuration.relayPin),
                 relay.configuration.activeHigh ? "Yes" : "No",
                 relay.hardwareApplied ? "APPLIED" : "NOT APPLIED",
                 relay.commandedOn ? "ON" : "OFF",
                 readOk ? (readBack == relay.commandedOn ? "  (readback matches)" : "  (READBACK MISMATCH)") : "");
    }

    ESP_LOGI(TAG, "--------------------------------------------------------------");
#endif
}


bool RelayController::configureGpio(RegisteredRelay& relay)
{
#ifdef ESP_PLATFORM
    const gpio_num_t pin = static_cast<gpio_num_t>(relay.configuration.relayPin);

    /*
     * The initial output level is set as part of the same gpio_config()
     * call that enables the pin as an output. The document additionally
     * specifies a physical 10 kOhm pull-down resistor on each relay
     * control line to guard against a spurious relay activation during
     * the ESP32 boot sequence, before this configuration call runs;
     * pull_down_en here is the matching software-side default for the
     * short window after this call while no explicit level has been
     * commanded yet.
     */
    gpio_config_t ioConfiguration{};
    ioConfiguration.pin_bit_mask = 1ULL << static_cast<unsigned int>(pin);
    ioConfiguration.mode = GPIO_MODE_OUTPUT;
    ioConfiguration.pull_up_en = GPIO_PULLUP_DISABLE;
    ioConfiguration.pull_down_en = GPIO_PULLDOWN_ENABLE;
    ioConfiguration.intr_type = GPIO_INTR_DISABLE;

    const esp_err_t configureResult = gpio_config(&ioConfiguration);
    if (configureResult != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config() failed for relay pin %u: %s",
                 static_cast<unsigned int>(relay.configuration.relayPin), esp_err_to_name(configureResult));
        return false;
    }

    relay.hardwareApplied = true;

    if (!writeGpioLevel(relay, relay.configuration.initialStateOn)) {
        ESP_LOGE(TAG, "Initial safe-state write failed for relay pin %u",
                 static_cast<unsigned int>(relay.configuration.relayPin));
        return false;
    }

    return true;
#else
    (void)relay;
    return false;
#endif
}


bool RelayController::writeGpioLevel(const RegisteredRelay& relay, bool on) const
{
#ifdef ESP_PLATFORM
    if (!relay.hardwareApplied) {
        return false;
    }

    /*
     * Translate the logical ON/OFF state through this relay's physical
     * polarity: an active-LOW board needs GPIO LOW to energise the relay
     * (Load ON), so the driven level is the logical inverse of `on`.
     */
    const bool driveHigh = relay.configuration.activeHigh ? on : !on;

    const esp_err_t result = gpio_set_level(
        static_cast<gpio_num_t>(relay.configuration.relayPin),
        driveHigh ? 1U : 0U
    );

    return result == ESP_OK;
#else
    (void)relay;
    (void)on;
    return false;
#endif
}


bool RelayController::readGpioLevel(const RegisteredRelay& relay, bool& on) const
{
#ifdef ESP_PLATFORM
    if (!relay.hardwareApplied) {
        return false;
    }

    const int level = gpio_get_level(static_cast<gpio_num_t>(relay.configuration.relayPin));

    /* Translate the physical level back through this relay's polarity. */
    on = relay.configuration.activeHigh ? (level != 0) : (level == 0);
    return true;
#else
    (void)relay;
    (void)on;
    return false;
#endif
}


} // namespace kilowatts
