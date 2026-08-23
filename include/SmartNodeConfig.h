#ifndef KILOWATTS_SMART_NODE_CONFIG_H
#define KILOWATTS_SMART_NODE_CONFIG_H

namespace kilowatts {
namespace SmartNodeConfig {

/*
 * Deliberately empty: the installation chooses the GPIO control pin for each
 * Load at configuration time. The current protocol field is named relayPin
 * because the prototype commonly uses relay modules, but the firmware only
 * configures/drives the GPIO and does not infer or verify what is connected
 * after it.
 */

} // namespace SmartNodeConfig
} // namespace kilowatts

#endif // KILOWATTS_SMART_NODE_CONFIG_H
