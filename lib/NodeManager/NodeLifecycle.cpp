#include "NodeLifecycle.h"

namespace kilowatts {


bool isValidNodeLifecycleTransition(NodeLifecycleState from, NodeLifecycleState to)
{
    switch (from) {
        case NodeLifecycleState::FACTORY:
            return to == NodeLifecycleState::UNCOMMISSIONED;

        case NodeLifecycleState::UNCOMMISSIONED:
            return to == NodeLifecycleState::DISCOVERED ||
                   to == NodeLifecycleState::CONFIGURING ||
                   to == NodeLifecycleState::DECOMMISSIONED;

        case NodeLifecycleState::DISCOVERED:
            return to == NodeLifecycleState::CONFIGURING ||
                   to == NodeLifecycleState::DECOMMISSIONED;

        case NodeLifecycleState::CONFIGURING:
            /*
             * CONFIGURING is only entered for a Node's first commissioning
             * (never for renaming an already-commissioned Node), so a
             * failed/timed-out attempt can unambiguously roll back to
             * UNCOMMISSIONED.
             */
            return to == NodeLifecycleState::COMMISSIONED ||
                   to == NodeLifecycleState::UNCOMMISSIONED ||
                   to == NodeLifecycleState::DECOMMISSIONED;

        case NodeLifecycleState::COMMISSIONED:
            return to == NodeLifecycleState::OPERATIONAL ||
                   to == NodeLifecycleState::DECOMMISSIONED;

        case NodeLifecycleState::OPERATIONAL:
            return to == NodeLifecycleState::DECOMMISSIONED;

        case NodeLifecycleState::DECOMMISSIONED:
            return to == NodeLifecycleState::UNCOMMISSIONED;
    }

    return false;
}


const char* toText(NodeLifecycleState state)
{
    switch (state) {
        case NodeLifecycleState::FACTORY:        return "factory";
        case NodeLifecycleState::UNCOMMISSIONED:  return "uncommissioned";
        case NodeLifecycleState::DISCOVERED:      return "discovered";
        case NodeLifecycleState::CONFIGURING:     return "configuring";
        case NodeLifecycleState::COMMISSIONED:    return "commissioned";
        case NodeLifecycleState::OPERATIONAL:     return "operational";
        case NodeLifecycleState::DECOMMISSIONED:  return "decommissioned";
    }

    return "unknown";
}


const char* toText(NodeRole role)
{
    switch (role) {
        case NodeRole::CENTRAL: return "central";
        case NodeRole::SMART:   return "smart";
    }

    return "unknown";
}


} // namespace kilowatts
