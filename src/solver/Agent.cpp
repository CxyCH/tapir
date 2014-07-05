/** @file Agent.cpp
 *
 * Contains the implementation of the Agent class.
 */
#include "solver/Agent.hpp"

#include "solver/BeliefNode.hpp"
#include "solver/BeliefTree.hpp"
#include "solver/Solver.hpp"

namespace solver {

Agent::Agent(Solver *solver) :
        solver_(solver),
        currentBelief_(solver_->getPolicy()->getRoot()) {
}

Solver *Agent::getSolver() const {
    return solver_;
}
std::unique_ptr<Action> Agent::getPreferredAction() const {
    return currentBelief_->getRecommendedAction();
}
BeliefNode *Agent::getCurrentBelief() const {
    return currentBelief_;
}

void Agent::updateBelief(Action const &action, Observation const &observation) {
    currentBelief_ = solver_->getPolicy()->createOrGetChild(currentBelief_, action, observation);
}
} /* namespace solver */