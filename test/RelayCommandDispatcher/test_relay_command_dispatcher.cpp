/**
 * @file test_relay_command_dispatcher.cpp
 * @brief Host-native correctness tests for OFF-before-ON dispatch ordering
 *        and command/acknowledgement tracking (Algorithm 4.6).
 *
 * This file uses a standard host int main(), not an ESP-IDF app_main(), so
 * it can be compiled and run by run_cpp_test.sh's plain g++ invocation.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
 */

#include "RelayCommandDispatcher.h"

#include <cstdio>

using kilowatts::Load;
using kilowatts::RelayCommandDispatcher;

namespace {

std::size_t passedChecks = 0U;
std::size_t failedChecks = 0U;

const char* recordResult(bool passed) {
    if (passed) {
        ++passedChecks;
        return "PASS";
    }

    ++failedChecks;
    return "FAIL";
}

bool reportCheck(const char* name, bool passed) {
    std::printf("%-78s %s\n", name, recordResult(passed));
    return passed;
}

void printSection(const char* title) {
    std::printf("\n======================================================================\n");
    std::printf("%s\n", title);
    std::printf("======================================================================\n");
}

const Load::MacAddress NODE_A = {0x02, 0x00, 0x00, 0x00, 0xD0, 0x0A};
const Load::MacAddress NODE_B = {0x02, 0x00, 0x00, 0x00, 0xD0, 0x0B};

using RelayTarget = RelayCommandDispatcher::RelayTarget;

RelayTarget makeTarget(const Load::MacAddress& mac, std::uint8_t relayPin, bool desiredOn, bool currentlyConfirmedOn) {
    return RelayTarget{mac, relayPin, desiredOn, currentlyConfirmedOn, /* currentlyConfirmedValid */ true};
}

RelayTarget makeTargetWithUnknownConfirmedState(const Load::MacAddress& mac, std::uint8_t relayPin, bool desiredOn) {
    /* currentlyConfirmedOn is deliberately set equal to desiredOn here, to prove validity (not the stale value) drives the no-op decision. */
    return RelayTarget{mac, relayPin, desiredOn, desiredOn, /* currentlyConfirmedValid */ false};
}

/**
 * TEST 1 - NO-OP TARGETS ARE DROPPED
 */
void testNoOpTargetsAreDropped() {
    printSection("TEST 1 - NO-OP TARGETS ARE DROPPED");

    std::vector<RelayTarget> targets = {
        makeTarget(NODE_A, 16U, true, true),    // already ON, target ON -> no-op
        makeTarget(NODE_A, 17U, false, false),  // already OFF, target OFF -> no-op
    };

    const std::vector<RelayTarget> dispatchOrder = RelayCommandDispatcher::buildDispatchOrder(targets);
    reportCheck("Two already-satisfied targets produce an empty dispatch order", dispatchOrder.empty());
}

/**
 * TEST 2 - OFF TRANSITIONS ARE DISPATCHED BEFORE ANY ON TRANSITION
 * Three ON transitions listed first in the input, then two OFF
 * transitions: the dispatch order must still place both OFF transitions
 * ahead of all three ON transitions.
 */
void testOffBeforeOn() {
    printSection("TEST 2 - OFF TRANSITIONS DISPATCHED BEFORE ANY ON TRANSITION");

    std::vector<RelayTarget> targets = {
        makeTarget(NODE_A, 16U, true, false),   // ON transition (highest Best-First priority)
        makeTarget(NODE_A, 17U, true, false),   // ON transition
        makeTarget(NODE_B, 16U, true, false),   // ON transition (lowest Best-First priority)
        makeTarget(NODE_A, 18U, false, true),   // OFF transition
        makeTarget(NODE_B, 17U, false, true),   // OFF transition
    };

    const std::vector<RelayTarget> dispatchOrder = RelayCommandDispatcher::buildDispatchOrder(targets);

    reportCheck("All five real transitions are present in the dispatch order", dispatchOrder.size() == 5U);

    bool sawAnOnTransitionBeforeAllOffTransitionsFinished = false;
    bool everyOffTransitionSeenSoFar = true;
    std::size_t offCount = 0U;
    std::size_t onCount = 0U;

    for (const RelayTarget& target : dispatchOrder) {
        if (!target.desiredOn) {
            ++offCount;
            if (onCount > 0U) {
                sawAnOnTransitionBeforeAllOffTransitionsFinished = true;
            }
        } else {
            ++onCount;
            everyOffTransitionSeenSoFar = everyOffTransitionSeenSoFar && (offCount == 2U);
        }
    }

    reportCheck("No ON transition appears before every OFF transition", !sawAnOnTransitionBeforeAllOffTransitionsFinished);
    reportCheck("Both OFF transitions were dispatched before the first ON transition", everyOffTransitionSeenSoFar);
    reportCheck("Both OFF transitions are present", offCount == 2U);
    reportCheck("All three ON transitions are present", onCount == 3U);
}

/**
 * TEST 3 - ON PHASE PRESERVES BEST-FIRST ADMISSION ORDER
 */
void testOnPhasePreservesOrder() {
    printSection("TEST 3 - ON PHASE PRESERVES BEST-FIRST ADMISSION ORDER");

    std::vector<RelayTarget> targets = {
        makeTarget(NODE_A, 16U, true, false),   // admitted 1st (best score)
        makeTarget(NODE_A, 17U, true, false),   // admitted 2nd
        makeTarget(NODE_B, 16U, true, false),   // admitted 3rd (worst score)
    };

    const std::vector<RelayTarget> dispatchOrder = RelayCommandDispatcher::buildDispatchOrder(targets);

    reportCheck("Dispatch order has exactly three ON transitions", dispatchOrder.size() == 3U);
    reportCheck("First dispatched ON transition is the first Best-First admission (Node A pin 16)",
                dispatchOrder.size() > 0U && dispatchOrder[0].relayPin == 16U &&
                dispatchOrder[0].nodeMacAddress == NODE_A);
    reportCheck("Second dispatched ON transition is the second Best-First admission (Node A pin 17)",
                dispatchOrder.size() > 1U && dispatchOrder[1].relayPin == 17U &&
                dispatchOrder[1].nodeMacAddress == NODE_A);
    reportCheck("Third dispatched ON transition is the third Best-First admission (Node B pin 16)",
                dispatchOrder.size() > 2U && dispatchOrder[2].relayPin == 16U &&
                dispatchOrder[2].nodeMacAddress == NODE_B);
}

/**
 * TEST 4 - COMMAND/ACKNOWLEDGEMENT TRACKING
 */
void testCommandTracking() {
    printSection("TEST 4 - COMMAND/ACKNOWLEDGEMENT TRACKING");

    RelayCommandDispatcher dispatcher;
    reportCheck("A new dispatcher starts with zero pending commands", dispatcher.getNumberOfPendingCommands() == 0U);

    const RelayTarget target = makeTarget(NODE_A, 16U, true, false);
    const std::uint32_t commandId = dispatcher.beginCommand(target, 1000U);

    reportCheck("beginCommand() returns a non-zero commandId", commandId != 0U);
    reportCheck("isPending() is true immediately after beginCommand()", dispatcher.isPending(commandId));
    reportCheck("getNumberOfPendingCommands() becomes 1", dispatcher.getNumberOfPendingCommands() == 1U);

    const RelayCommandDispatcher::PendingCommand* pending = dispatcher.findPendingCommand(commandId);
    reportCheck("findPendingCommand() returns the correct record",
                pending != nullptr && pending->relayPin == 16U && pending->nodeMacAddress == NODE_A &&
                pending->desiredOn && pending->dispatchedAtMilliseconds == 1000U);

    const std::uint32_t secondCommandId = dispatcher.beginCommand(makeTarget(NODE_A, 17U, false, true), 1000U);
    reportCheck("A second beginCommand() allocates a distinct commandId", secondCommandId != commandId);
    reportCheck("Two commands are now pending", dispatcher.getNumberOfPendingCommands() == 2U);

    reportCheck("completeCommand() succeeds for a pending commandId", dispatcher.completeCommand(commandId));
    reportCheck("isPending() is false after completeCommand()", !dispatcher.isPending(commandId));
    reportCheck("One command remains pending", dispatcher.getNumberOfPendingCommands() == 1U);

    reportCheck("completeCommand() on an already-completed commandId is rejected", !dispatcher.completeCommand(commandId));
    reportCheck("completeCommand() on a never-issued commandId is rejected", !dispatcher.completeCommand(99999U));

    dispatcher.clearPendingCommands();
    reportCheck("clearPendingCommands() removes every remaining pending command",
                dispatcher.getNumberOfPendingCommands() == 0U);
}

/**
 * TEST 5 - EXPIRED COMMAND DETECTION
 */
void testExpiredCommandDetection() {
    printSection("TEST 5 - EXPIRED COMMAND DETECTION");

    RelayCommandDispatcher dispatcher;

    const std::uint32_t staleCommandId = dispatcher.beginCommand(makeTarget(NODE_A, 16U, true, false), 1000U);
    const std::uint32_t freshCommandId = dispatcher.beginCommand(makeTarget(NODE_A, 17U, true, false), 4500U);

    const std::vector<RelayCommandDispatcher::PendingCommand> expired = dispatcher.findExpiredCommands(5000U, 2000U);

    reportCheck("Exactly one command exceeded the timeout (5000 - 1000 = 4000 > 2000)", expired.size() == 1U);
    reportCheck("The expired command is the stale one",
                expired.size() == 1U && expired[0].commandId == staleCommandId);
    reportCheck("The fresh command (5000 - 4500 = 500 <= 2000) is not reported expired",
                dispatcher.isPending(freshCommandId));
    reportCheck("findExpiredCommands() does not remove anything itself",
                dispatcher.getNumberOfPendingCommands() == 2U);
}

/**
 * TEST 6 - UNKNOWN CONFIRMED STATE IS NEVER TREATED AS A NO-OP
 * A target whose currentlyConfirmedOn happens to already equal desiredOn
 * must still be dispatched when currentlyConfirmedValid is false — an
 * unknown physical state is never assumed to already match what is
 * wanted (Section "Unknown Relay State").
 */
void testUnknownConfirmedStateIsNeverANoOp() {
    printSection("TEST 6 - UNKNOWN CONFIRMED STATE IS NEVER A NO-OP");

    std::vector<RelayTarget> targets = {
        makeTarget(NODE_A, 16U, true, true),                             // valid, already matches -> no-op
        makeTargetWithUnknownConfirmedState(NODE_A, 17U, true),          // unknown, "matches" on paper -> must still dispatch
        makeTargetWithUnknownConfirmedState(NODE_A, 18U, false),         // unknown, "matches" on paper -> must still dispatch
    };

    const std::vector<RelayTarget> dispatchOrder = RelayCommandDispatcher::buildDispatchOrder(targets);

    reportCheck("The genuinely-confirmed no-op (pin16) is dropped", dispatchOrder.size() == 2U);

    bool sawPin17 = false;
    bool sawPin18 = false;
    for (const RelayTarget& target : dispatchOrder) {
        reportCheck("Dropped no-op target (pin16) never appears in the dispatch order", target.relayPin != 16U);
        if (target.relayPin == 17U) sawPin17 = true;
        if (target.relayPin == 18U) sawPin18 = true;
    }

    reportCheck("The unknown-state ON target (pin17) is still dispatched despite matching on paper", sawPin17);
    reportCheck("The unknown-state OFF target (pin18) is still dispatched despite matching on paper", sawPin18);
}

} // namespace

int main() {
    std::printf("KILOWATTS RELAYCOMMANDDISPATCHER HOST TEST REPORT\n");
    std::printf("Author: Chalwe Silas\n");
    std::printf("Programme: Final-Year Computer Engineering\n");
    std::printf("Institution: The Copperbelt University\n");

    testNoOpTargetsAreDropped();
    testOffBeforeOn();
    testOnPhasePreservesOrder();
    testCommandTracking();
    testExpiredCommandDetection();
    testUnknownConfirmedStateIsNeverANoOp();

    printSection("FINAL TEST SUMMARY");
    std::printf("Passed checks: %zu\n", passedChecks);
    std::printf("Failed checks: %zu\n", failedChecks);
    std::printf("OVERALL RESULT: %s\n", failedChecks == 0U ? "PASS" : "FAIL");

    return failedChecks == 0U ? 0 : 1;
}
