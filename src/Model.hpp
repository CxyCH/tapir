#ifndef MODEL_HPP
#define MODEL_HPP

#include <memory>                       // for unique_ptr
#include <ostream>                      // for ostream
#include <vector>                       // for vector

#include "defs.hpp"                     // for RandomGenerator
#include "Action.hpp"                   // for Action
#include "ChangeType.hpp"               // for ChangeType
#include "Observation.hpp"              // for Observation
class State;

class Model {
  public:
    /** Represents the results of a step in the model, including the next state,
     * observation, and reward.
     */
    struct StepResult {
        Action action;
        Observation observation;
        double immediateReward;
        std::unique_ptr<State> nextState;
        bool isTerminal;
    };

    Model(RandomGenerator *randGen) : randGen(randGen) {
    }

    /** Destructor must be virtual */
    virtual ~Model() = default;
    Model(Model const &) = delete;
    Model(Model &&) = delete;
    Model &operator=(Model const &) = delete;
    Model &operator=(Model &&) = delete;

    /* ---------- Virtual getters for important model parameters  ---------- */
    // POMDP parameters
    /** Returns the POMDP discount factor. */
    virtual double getDiscount() = 0;
    /** Returns the # of actions for this POMDP. */
    virtual unsigned long getNActions() = 0;
    /** Returns the # of observations f {or this POMDP. */
    virtual unsigned long getNObservations() = 0;
    /** Returns the number of state variables for this PODMP. */
    virtual unsigned long getNStVars() = 0;
    /** Returns a lower bound on the q-value. */
    virtual double getMinVal() = 0;
    /** Returns an upper bound on the q-value. */
    virtual double getMaxVal() = 0;

    // SBT algorithm parameters
    /** Returns the maximum number of particles */
    virtual unsigned long getNParticles() = 0;
    /** Returns the maximum number of trials to run. */
    virtual long getMaxTrials() = 0;
    /** Returns the lowest cumulative discount before the  */
    virtual double getDepthTh() = 0;
    /** Returns the exploration coefficient used for rollouts.
     * ??
     */
    virtual double getExploreCoef() = 0;
    /** Returns the maximum number of nodes to check when searching
     * for a nearest-neighbour belief node.
     */
    virtual long getMaxDistTry() = 0;
    /** Returns the smallest allowable distance when searching for
     * a nearest-neighbour belief node.
     */
    virtual double getDistTh() = 0;

    /* --------------- Start virtual functions ----------------- */
    /** Samples an initial state from the belief vector. */
    virtual std::unique_ptr<State> sampleAnInitState() = 0;
    /** Returns true iff the given state is terminal. */
    virtual bool isTerm(State const &state) = 0;
    /** Approximates the q-value of a state */
    virtual double solveHeuristic(State const &state) = 0;
    /** Returns the default q-value */
    virtual double getDefaultVal() = 0;

    /** Generates the next state, an observation, and the reward. */
    virtual StepResult generateStep(State const &state, Action const &action) = 0;
    /** Returns the reward for the given state. */
    virtual double getReward(State const &state) = 0;
    /** Returns the reward for the given state and action. */
    virtual double getReward(State const &state, Action const &action) = 0;

    /** Generates new state particles based on the state particles of the
     * previous node, as well as on the action and observation.
     */
    virtual std::vector<std::unique_ptr<State> > generateParticles(Action const &action,
            Observation const &obs, std::vector<State *> const &previousParticles) = 0;
    /** Generates new state particles based only on the previous action and
     * observation, assuming a poorly-informed prior over previous states.
     *
     * This should only be used if the previous belief turns out to be
     * incompatible with the current observation.
     */
    virtual std::vector<std::unique_ptr<State> > generateParticles(Action const &,
            Observation const &obs) = 0;

    /** Loads model changes from the given file. */
    virtual std::vector<long> loadChanges(char const *changeFilename) = 0;

    /** Retrieves the range of states that is affected by the change. */
    virtual void update(long time, std::vector<std::unique_ptr<State> > *affectedRange,
                        std::vector<ChangeType> *typeOfChanges) = 0;

    /** Generates a modified version of the given sequence of states, between
     * the start and end indices.
     *
     * Should return true if modifications have actually been made, and false
     * otherwise.
     */
    virtual bool modifStSeq(std::vector<State const *> const &states,
                            long startAffectedIdx, long endAffectedIdx,
                            std::vector<std::unique_ptr<State> > *modifStSeq,
                            std::vector<Action> *modifActSeq,
                            std::vector<Observation> *modifObsSeq,
                            std::vector<double> *modifRewSeq) = 0;

    virtual void dispAct(Action const &action, std::ostream &os) = 0;
    virtual void dispObs(Observation const &obs, std::ostream &os) = 0;
    virtual void drawEnv(std::ostream &os) = 0;
    virtual void drawState(State const &state, std::ostream &os) = 0;
  protected:
    RandomGenerator *randGen;
};

#endif
