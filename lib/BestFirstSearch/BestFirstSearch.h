/**
 * @file BestFirstSearch.h
 * @brief Declares the Best-First Search used to select Auto candidate
 *        Load objects (Sections 4.6.2-4.6.4).
 *
 * BestFirstSearch receives Auto candidate Load objects and the current
 * planning-cycle electrical state (battery State of Charge, the power
 * available to Auto Loads this cycle, and the electrical limits a
 * candidate may not exceed). It does not measure or calculate any of
 * those values itself — battery-state estimation, power-budget
 * calculation, Fixed-Load allocation, schedule evaluation, relay
 * actuation and communication are all handled by other modules, outside
 * this class. BestFirstSearch's own responsibility is exactly: score
 * each candidate (g(i), h(i), f(i)), maintain the OPEN min-priority
 * queue, apply the electrical constraint guard, and sequentially admit
 * or reject candidates while updating its own P_remaining, P_committed
 * and per-branch committed power.
 *
 * A "branch" here is Section 4.6.2.5's branch circuit b(i): one
 * relay-controlled DC output circuit, physically identified by the Node
 * that switches it plus the relay pin used to switch it (BranchId, below).
 * That identity is supplied by the caller for each candidate, together
 * with that branch's committed power and maximum current — the maximum
 * current is a configured Branch property, never part of the identity
 * itself, so re-configuring a Branch's current limit never creates a new
 * Branch. BestFirstSearch only tracks the committed power booked against
 * each BranchId it is told about; it does not decide what a branch
 * corresponds to in the physical wiring beyond that identity.
 *
 * OPEN is implemented as a binary min-heap ordered by f(i), so the
 * candidate with the smallest (most desirable) score is extracted first.
 *
 * @author Chalwe Silas
 * @programme Final-Year Computer Engineering
 * @institution The Copperbelt University
 * @date 13 August 2026
 */

#ifndef KILOWATTS_BEST_FIRST_SEARCH_H
#define KILOWATTS_BEST_FIRST_SEARCH_H

#include "Load.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kilowatts {


class BestFirstSearch {

public:

    /**
     * Identifies one physical electrical branch circuit, b(i) in Section
     * 4.6.2.5: the relay-controlled DC output that feeds a Load. A Branch
     * is one relay channel belonging to one Node, so its identity is
     * exactly that Node's MAC address plus the relay pin switching it —
     * the same physical addressing already used for Load::Id, since in
     * the current Kilowatts hardware one relay channel feeds exactly one
     * Load. Branch and Load are still two separate concepts (a Branch is
     * the circuit, a Load is the appliance on it); BranchId simply
     * reuses the same {Node MAC, relay pin} addressing.
     *
     * BestFirstSearch never derives a BranchId from a Load or Node
     * itself — it is always supplied by the caller through
     * registerBranch() and addLoad(). Maximum current is deliberately not
     * part of this identity: it is Branch configuration, supplied
     * separately to registerBranch(), so changing a Branch's configured
     * current limit never creates a new Branch.
     */
    struct BranchId {
        Load::MacAddress nodeMacAddress;
        std::uint8_t relayPin;

        bool operator==(const BranchId& other) const
        {
            return nodeMacAddress == other.nodeMacAddress &&
                   relayPin == other.relayPin;
        }

        bool operator!=(const BranchId& other) const
        {
            return !(*this == other);
        }
    };


    /**
     * Rejection reason codes stored for a candidate that failed the
     * electrical constraint guard (Equation 4.25 / Algorithm 4.4). The
     * numeric values match the order the guard checks them in.
     */
    static constexpr std::uint8_t NONE = 0U;
    static constexpr std::uint8_t LOW_BATTERY = 1U;
    static constexpr std::uint8_t POWER_BUDGET_EXCEEDED = 2U;
    static constexpr std::uint8_t BATTERY_CURRENT_LIMIT = 3U;
    static constexpr std::uint8_t MAIN_LIMIT_EXCEEDED = 4U;
    static constexpr std::uint8_t BRANCH_LIMIT_EXCEEDED = 5U;


    /**
     * Policy weights controlling the physical-cost g(i) (Equation 4.30)
     * and heuristic-cost h(i) (Equation 4.32) terms of the complete
     * Best-First score f(i) = g(i) + h(i) (Equation 4.33).
     *
     * maximumAllowedPriority is W_max, the priority scale
     * Load::getPriority() values are normalized against to produce q_i
     * (Equation 4.31). These are policy/configuration values, not
     * physical measurements — BestFirstSearch validates them but does
     * not invent production defaults for them.
     */
    struct Weights {
        float runningPowerWeight;    // w_P
        float startupPowerWeight;    // w_S
        float batteryStressWeight;   // w_B
        float priorityWeight;        // w_Q
        float scheduleWeight;        // w_T
        std::uint16_t maximumAllowedPriority; // W_max
    };


    /**
     * Battery and electrical-limit inputs for one planning cycle
     * (Sections 4.6.2.2-4.6.2.5). Every value here is measured or
     * derived elsewhere — battery-state estimation and power-budget
     * preparation, a later phase — and supplied unchanged.
     * BestFirstSearch does not measure SoC, does not read INA219, and
     * does not calculate P_battery,max or I_main,max itself.
     *
     * totalAvailablePowerWatts is P_available (Equation 4.14): the total
     * safe planning power for this cycle, before Fixed-load allocation.
     * It is used only as the constant normalization denominator for
     * p_i/s_i (Equations 4.27-4.28) — it is exactly
     * AvailablePowerManager::getTotalAvailablePowerWatts().
     *
     * powerAvailableForAutoLoadsWatts is P_remaining's starting value
     * (Equation 4.16: P_available - P_fixed): the Auto-load allocation
     * budget this search sequentially consumes as candidates are
     * admitted (Algorithm 4.5). It is exactly
     * AvailablePowerManager::getPowerAvailableForAutoLoadsWatts(), and is
     * a genuinely different quantity from totalAvailablePowerWatts
     * whenever Fixed ON Loads commit part of the total budget.
     *
     * initialCommittedPowerWatts is P_committed's starting value: the
     * steady-state power already flowing on the main DC bus before this
     * search admits anything — normally the Fixed ON Running Power
     * already calculated by AvailablePowerManager. BestFirstSearch owns
     * P_committed from this point on and increases it as Auto Loads are
     * admitted (Equation 4.36).
     */
    struct ElectricalPlanningState {
        float stateOfChargePercent;            // SoC        (Equation 4.8)
        float minimumStateOfChargePercent;     // SoC_min    (Section 4.6.2.2)
        float warningStateOfChargePercent;     // SoC_warn   (Equation 4.29)
        float batteryBusVoltageVolts;          // V_B        (Equations 4.18-4.19)
        float maximumBatteryPowerWatts;        // P_battery,max (Equation 4.12)
        float maximumMainCurrentAmps;          // I_main,max (Section 4.6.2.5)
        float totalAvailablePowerWatts;        // P_available (Equation 4.14)
        float powerAvailableForAutoLoadsWatts; // P_remaining at the start (Equation 4.16)
        float initialCommittedPowerWatts;      // P_committed at the start
    };


    /**
     * Creates an empty Best-First Search.
     *
     * There is no artificial maximum number of Loads or branches in this
     * class.
     */
    BestFirstSearch();


    /**
     * Sets the policy weights used by the Best-First score.
     *
     * Rejected once a search has started, or when any weight is
     * non-finite/negative, or when maximumAllowedPriority is zero.
     */
    bool setSearchScoreWeights(const Weights& weights);


    /**
     * Starts a new planning cycle using the supplied electrical state.
     *
     * Rejected when the weights are not configured, when
     * planningState contains a non-finite value, a percentage outside
     * 0-100, a battery bus voltage that is not strictly positive, a
     * negative power/current limit, or warningStateOfChargePercent that
     * does not lie strictly above minimumStateOfChargePercent (the
     * battery-stress denominator in Equation 4.29 would otherwise be
     * zero or negative). Also rejected while a previously started search
     * has not yet been run() or reset.
     */
    bool startSearch(const ElectricalPlanningState& planningState);


    /**
     * Registers one electrical branch's starting committed power and
     * maximum current for the current search. Must be called once per
     * BranchId, after startSearch() and before any addLoad() referencing
     * that BranchId.
     *
     * Rejected when the search has not started/has completed, the
     * BranchId is already registered, or either value is non-finite or
     * negative.
     */
    bool registerBranch(
        BranchId branchId,
        float initialCommittedPowerWatts,
        float maximumCurrentAmps
    );


    /**
     * Adds one Auto candidate Load to the current search on the given
     * branch, together with its schedule future-penalty r_i (Equation
     * 4.32) already prepared by LoadScheduleEvaluator.
     *
     * Rejected when the search has not started/has completed, the Load
     * was already added, branchId was not registered via registerBranch()
     * first, the Load's running/startup power or priority are invalid, or
     * scheduleFuturePenalty is not finite or outside [0, 1].
     */
    bool addLoad(
        const Load& load,
        BranchId branchId,
        float scheduleFuturePenalty
    );


    /**
     * Runs Best-First Search over every Auto candidate that was added
     * (Algorithm 4.5): scores each candidate once, orders them in the
     * OPEN min-heap by f(i), then repeatedly extracts the smallest score
     * and applies the electrical constraint guard.
     */
    bool run();


    /**
     * Clears the Loads, branches and results from the current search.
     *
     * The search score weights remain configured.
     */
    void resetSearch();


    /** Returns the number of Load objects currently added to the search. */
    std::size_t getNumberOfLoadsAdded() const;


    /** Returns the original Load object at loadIndex, or nullptr. */
    const Load* getLoad(std::size_t loadIndex) const;


    /** Returns true when BestFirstSearch admitted the Load (x_i = 1). */
    bool isLoadSelectedToBeOn(std::size_t loadIndex) const;


    /**
     * Returns the reason a Load was not admitted (NONE when it was, or
     * when the search has not run yet).
     */
    std::uint8_t getLoadSelectionRejectionReason(
        std::size_t loadIndex
    ) const;


    /**
     * Returns how many candidates run() actually admitted (x_i = 1) —
     * distinct from getNumberOfLoadsAdded(), which counts every candidate
     * regardless of outcome.
     */
    std::size_t getNumberOfAdmittedLoads() const;


    /**
     * Returns the admitted candidate at admissionPosition, in the exact
     * order run() extracted and admitted it from the OPEN min-heap —
     * admissionPosition 0 is the first (smallest f(i)) Load admitted,
     * admissionPosition getNumberOfAdmittedLoads()-1 is the last.
     *
     * This is the order the document requires relay ON commands be
     * dispatched in ("Loads selected for ON operation are then enabled
     * in Best-First order"): getLoad()/isLoadSelectedToBeOn() alone
     * cannot reconstruct it, since those are indexed by addLoad() call
     * order, not by admission order — a caller that rebuilt ON order by
     * re-traversing candidates in addLoad()/registry order would silently
     * discard the Best-First ordering this search just computed.
     *
     * Returns nullptr when admissionPosition is out of range, or before
     * run() has completed.
     */
    const Load* getAdmittedLoad(std::size_t admissionPosition) const;


    /**
     * One candidate's inputs to the electrical constraint guard
     * (Equations 4.20-4.25 / Algorithm 4.4), supplied directly rather
     * than read from a live search — used both internally by run() and
     * externally by a caller re-verifying feasibility immediately before
     * actuation, against a freshly re-read electrical snapshot, without
     * running the whole search again (Section "Add The Documented Safety
     * Re-Check Immediately Before Each ON Command").
     */
    struct FeasibilityInputs {
        float stateOfChargePercent;        // SoC
        float minimumStateOfChargePercent; // SoC_min
        float candidateRunningPowerWatts;  // P_i
        float candidatePeakPowerWatts;     // P_i_peak
        float remainingPowerWatts;         // P_remaining (live, not the search's own frozen post-run value)
        float committedPowerWatts;         // P_committed (live)
        float maximumBatteryPowerWatts;    // P_battery,max
        float batteryBusVoltageVolts;      // V_B
        float maximumMainCurrentAmps;      // I_main,max
        float branchCommittedPowerWatts;   // P_branch (live)
        float branchMaximumCurrentAmps;    // I_branch,max
    };


    /**
     * Applies the exact Algorithm 4.4 constraint guard (Equations
     * 4.20-4.25, in the documented check order) to inputs and returns
     * NONE when feasible, otherwise the first violated constraint's
     * rejection reason. Pure function — reads nothing from this object's
     * own state, so it is safe to call at any time, including outside an
     * active search, and is exactly what a pre-actuation safety re-check
     * needs: rerun this one function against a freshly re-read snapshot,
     * not the whole O(n log n) search.
     */
    static std::uint8_t checkFeasibility(const FeasibilityInputs& inputs);


    /** Returns P_available: the constant normalization denominator this search was started with. */
    float getTotalAvailablePowerWatts() const;

    /** Returns P_remaining: Auto-load power not yet committed this search. */
    float getRemainingPowerWatts() const;

    /** Returns P_committed: total steady-state power committed so far this search. */
    float getCommittedPowerWatts() const;

    /** Returns P_branch for branchId, or 0 when branchId was never registered. */
    float getBranchCommittedPowerWatts(BranchId branchId) const;

    /** Returns B, the battery-stress term computed once at startSearch() (Equation 4.29). */
    float getBatteryStressTerm() const;


    /** Returns p_i, the normalized running-power term (Equation 4.27). */
    float getLoadRunningPowerRatio(std::size_t loadIndex) const;

    /** Returns s_i, the normalized startup-power term (Equation 4.28). */
    float getLoadStartupPowerRatio(std::size_t loadIndex) const;

    /** Returns q_i, the normalized user-priority term (Equation 4.31). */
    float getLoadPriorityRatio(std::size_t loadIndex) const;

    /** Returns g(i), the physical cost (Equation 4.30). */
    float getLoadPhysicalCost(std::size_t loadIndex) const;

    /** Returns h(i), the heuristic preference cost (Equation 4.32). */
    float getLoadHeuristicCost(std::size_t loadIndex) const;

    /** Returns f(i) = g(i) + h(i), the complete Best-First score (Equation 4.33). */
    float getLoadFinalScore(std::size_t loadIndex) const;


private:

    /**
     * One electrical branch's running committed power and current limit,
     * tracked internally against the caller-supplied BranchId.
     */
    struct BranchState {
        BranchId branchId;
        float committedPowerWatts; // P_branch, updated as Loads are admitted
        float maximumCurrentAmps;  // I_branch,max
    };


    bool areWeightsValid(const Weights& weights) const;

    bool isElectricalPlanningStateValid(
        const ElectricalPlanningState& planningState
    ) const;

    bool isCurrentSearchStateValid() const;

    bool isLoadValidForSearch(std::size_t loadIndex) const;

    bool isLoadAlreadyAdded(const Load& load) const;

    BranchState* findBranchState(BranchId branchId);

    const BranchState* findBranchState(BranchId branchId) const;


    /**
     * Candidate Evaluation Function (Algorithm 4.3).
     */
    float calculateRunningPowerRatio(std::size_t loadIndex) const;

    float calculateStartupPowerRatio(std::size_t loadIndex) const;

    float calculatePriorityRatio(std::size_t loadIndex) const;

    float calculatePhysicalCost(std::size_t loadIndex) const;

    float calculateHeuristicCost(std::size_t loadIndex) const;

    float calculateFinalScore(std::size_t loadIndex) const;

    void calculateAndStoreLoadScores(std::size_t loadIndex);


    /**
     * Constraint Guard (Algorithm 4.4 / Equation 4.25). Returns NONE
     * when candidate loadIndex is feasible, otherwise the first
     * violated constraint's rejection reason, checked in the order
     * SoC reserve, power budget, battery current limit, main-bus
     * startup current, branch startup current.
     */
    std::uint8_t checkFeasibility(std::size_t loadIndex) const;


    /**
     * OPEN binary min-heap operations, ordered by loadFinalScores_.
     */
    void clearOpenSet();

    void insertLoadIntoOpenSet(std::size_t loadIndex);

    std::size_t extractBestLoadFromOpenSet();

    void moveLoadUpOpenSetUntilOrdered(std::size_t openSetPosition);

    void moveLoadDownOpenSetUntilOrdered(std::size_t openSetPosition);

    bool doesLeftLoadHaveBetterSearchOrder(
        std::size_t leftLoadIndex,
        std::size_t rightLoadIndex
    ) const;


    /**
     * Sequential Allocation of Remaining Capacity (Algorithm 4.5,
     * Equations 4.34-4.37): admits loadIndex and updates P_remaining,
     * P_committed and its branch's committed power.
     */
    void markLoadSelectedToBeOn(std::size_t loadIndex);

    /** Records that loadIndex was not admitted, with its rejection reason. */
    void markLoadNotSelected(
        std::size_t loadIndex,
        std::uint8_t loadSelectionRejectionReason
    );


    static float limitValueToRange(
        float value,
        float minimumAllowedValue,
        float maximumAllowedValue
    );

    static bool isFiniteAndNonNegative(float value);

    static bool isFinitePercentage(float value);


    /**
     * Configuration state.
     */
    bool searchScoreWeightsConfigured_;
    Weights weights_;


    /**
     * Search lifecycle state.
     */
    bool searchHasStarted_;
    bool searchHasCompleted_;


    /**
     * Electrical state supplied to startSearch(). Constant for the whole
     * search except committedPowerWatts_, which is BestFirstSearch's own
     * evolving P_committed (Equation 4.36).
     */
    ElectricalPlanningState planningState_;
    float remainingPowerWatts_;   // P_remaining
    float committedPowerWatts_;   // P_committed
    float batteryStressTerm_;     // B, computed once per search (Equation 4.29)


    /**
     * Electrical branches registered for this search.
     */
    std::vector<BranchState> branches_;


    /**
     * Candidate Auto Loads participating in this search. No separate
     * Load ID or extra Load reference is created.
     */
    std::vector<const Load*> loads_;

    /** Branch each candidate belongs to, parallel to loads_. */
    std::vector<BranchId> loadBranchIds_;

    /** r_i supplied for each candidate, parallel to loads_. */
    std::vector<float> loadScheduleFuturePenalties_;


    /**
     * Scores calculated internally for each candidate (Algorithm 4.3).
     */
    std::vector<float> loadRunningPowerRatios_;   // p_i
    std::vector<float> loadStartupPowerRatios_;   // s_i
    std::vector<float> loadPriorityRatios_;       // q_i
    std::vector<float> loadPhysicalCosts_;        // g(i)
    std::vector<float> loadHeuristicCosts_;       // h(i)
    std::vector<float> loadFinalScores_;          // f(i)


    /**
     * Final decision and rejection reason for each candidate.
     */
    std::vector<std::uint8_t> loadSelectedToBeOn_;
    std::vector<std::uint8_t> loadSelectionRejectionReasons_;


    /**
     * OPEN binary min-heap. Each value is an index into loads_.
     */
    std::vector<std::size_t> openSetLoadIndexes_;


    /**
     * Indexes into loads_, in the exact order run() extracted and
     * admitted them from OPEN — the Best-First admission order
     * getAdmittedLoad() reconstructs. Populated only for admitted
     * candidates (markLoadSelectedToBeOn()), never for rejected ones.
     */
    std::vector<std::size_t> admittedLoadIndexesInOrder_;
};


} // namespace kilowatts

#endif // KILOWATTS_BEST_FIRST_SEARCH_H
