/**
 * @file NodeLifecycle.cpp
 * @brief Implements the commissioning lifecycle transition rules.
 *
 * Plain, hardware-free C++ with no ESP-IDF dependency at all - always
 * compiled and fully host-testable (see test/NodeLifecycle/).
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 14 August 2026
 */

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
             * A failed/timed-out commissioning attempt rolls back to
             * UNCOMMISSIONED - this state is only ever entered from
             * UNCOMMISSIONED/DISCOVERED for a Node's first commissioning
             * (see NodeCommissioningRegistry::beginCommissioning()), never
             * for renaming an already-commissioned Node, so "failure means
             * uncommissioned" is unambiguous here.
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
