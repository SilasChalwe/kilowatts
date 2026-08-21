#ifndef KILOWATTS_SMART_NODE_CONFIG_H
#define KILOWATTS_SMART_NODE_CONFIG_H

namespace kilowatts {
namespace SmartNodeConfig {

/*
 * Deliberately empty: which GPIOs are safe relay outputs on a given board
 * is now entirely the installer's judgment call, made per Load through the
 * CONFIGURE_LOAD MQTT command (see ConfigCommandRequest::relayPin) rather
 * than a compiled-in per-board whitelist here.
 */

} // namespace SmartNodeConfig
} // namespace kilowatts

#endif // KILOWATTS_SMART_NODE_CONFIG_H
