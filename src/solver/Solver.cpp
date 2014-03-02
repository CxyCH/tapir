#include "Solver.hpp"

#include <cmath>                        // for pow, exp
#include <ctime>                        // for clock, clock_t, CLOCKS_PER_SEC

#include <algorithm>                    // for max
#include <iostream>                     // for operator<<, cerr, ostream, basic_ostream, endl, basic_ostream<>::__ostream_type, cout
#include <limits>
#include <memory>                       // for unique_ptr
#include <random>                       // for uniform_int_distribution, bernoulli_distribution
#include <set>                          // for set, _Rb_tree_const_iterator, set<>::iterator
#include <tuple>                        // for tie, tuple
#include <type_traits>                  // for remove_reference<>::type
#include <utility>                      // for move, make_pair, pair
#include <vector>                       // for vector, vector<>::iterator, vector<>::reverse_iterator

#include "global.hpp"                     // for RandomGenerator

#include "geometry/Action.hpp"                   // for Action
#include "geometry/Observation.hpp"              // for Observation
#include "geometry/State.hpp"                    // for State, operator<<

#include "mappings/ActionMapping.hpp"
#include "mappings/ObservationMapping.hpp"

#include "BeliefNode.hpp"               // for BeliefNode, BeliefNode::startTime
#include "BeliefTree.hpp"               // for BeliefTree
#include "ChangeFlags.hpp"               // for ChangeFlags, ChangeFlags::UNCHANGED, ChangeFlags::ADDOBSERVATION, ChangeFlags::ADDOBSTACLE, ChangeFlags::ADDSTATE, ChangeFlags::DELSTATE, ChangeFlags::REWARD, ChangeFlags::TRANSITION
#include "Histories.hpp"                // for Histories
#include "HistoryEntry.hpp"             // for HistoryEntry
#include "HistorySequence.hpp"          // for HistorySequence
#include "Model.hpp"                    // for Model::StepResult, Model
#include "StateInfo.hpp"                // for StateInfo
#include "StatePool.hpp"                // for StatePool

#include "serialization/Serializer.hpp"               // for Serializer

#include "indexing/RTree.hpp"
#include "indexing/SpatialIndexVisitor.hpp"

using std::cerr;
using std::cout;
using std::endl;

namespace solver {
Solver::Solver(RandomGenerator *randGen, std::unique_ptr<Model> model) :
    serializer_(nullptr),
    randGen_(randGen),
    model_(std::move(model)),
    actionPool_(model_->createActionPool()),
    observationPool_(model_->createObservationPool()),
    allStates_(std::make_unique<StatePool>(model_->createStateIndex())),
    allHistories_(std::make_unique<Histories>()),
    policy_(std::make_unique<BeliefTree>()),
    historyCorrector_(model_->createHistoryCorrector()),
    lastRolloutMode_(ROLLOUT_RANDHEURISTIC),
    heuristicExploreCoefficient_(this->model_->getHeuristicExploreCoefficient()),
    timeUsedPerHeuristic_{ 1.0, 1.0 },
    heuristicWeight_{ 1.0, 1.0 },
    heuristicProbability_{ 0.5, 0.5 },
    heuristicUseCount_{ 1, 1 } {
}

// Default destructor, not in .hpp
Solver::~Solver() {
}

void Solver::initialize() {
    actionPool_->observationPool_ = observationPool_.get();
    observationPool_->actionPool_ = actionPool_.get();
    policy_->setRoot(std::make_unique<BeliefNode>(
            actionPool_->createActionMapping()));
    historyCorrector_->setSolver(this);
}

void Solver::setSerializer(Serializer *serializer) {
    serializer_ = serializer;
}

void Solver::genPol(long maxTrials, long maximumDepth) {
    // Start expanding the tree.
    for (long i = 0; i < maxTrials; i++) {
        singleSearch(model_->getDiscountFactor(), maximumDepth);
    }
}

void Solver::singleSearch(double discountFactor, long maximumDepth) {
    StateInfo *stateInfo = allStates_->createOrGetInfo(*model_->sampleAnInitState());
    singleSearch(
            policy_->getRoot(), stateInfo, 0, discountFactor, maximumDepth);
}

void Solver::singleSearch(BeliefNode *startNode, StateInfo *startStateInfo,
        long startDepth, double discountFactor, long maximumDepth) {
    HistorySequence *sequence = allHistories_->addNew(startDepth);
    HistoryEntry *entry = sequence->addEntry(startStateInfo,
            std::pow(discountFactor, startDepth));
    entry->registerNode(startNode);
    continueSearch(sequence, discountFactor, maximumDepth);
}

void Solver::continueSearch(HistorySequence *sequence,
        double discountFactor, long maximumDepth) {
    HistorySequence *currHistSeq = sequence;
    HistoryEntry *currHistEntry = sequence->getEntry(
            sequence->histSeq_.size() - 1);
    double currentDiscount = currHistEntry->discount_;
    BeliefNode *currNode = currHistEntry->owningBeliefNode_;

    BeliefNode *sequenceRoot = sequence->getEntry(0)->owningBeliefNode_;
    double initialRootQValue = sequenceRoot->getBestMeanQValue();

    bool rolloutUsed = false;
    bool done = false;

    long currentDepth = currHistSeq->startDepth_ + currHistEntry->entryId_ + 1;
    while (!done && currentDepth <= maximumDepth) {
        currentDepth++;
        Model::StepResult result;
        double qVal = 0;
        if (!currNode->hasActionToTry()) {
            // If all actions have been attempted, use UCB
            std::unique_ptr<Action> action = currNode->getSearchAction(
                        model_->getUcbExploreCoefficient());
            result = model_->generateStep(*currHistEntry->getState(), *action);
            done = result.isTerminal;
        } else {
            // Otherwise use the rollout method
            std::tie(result, qVal) = getRolloutAction(currNode,
                        *currHistEntry->getState(), currentDiscount,
                        discountFactor);
            rolloutUsed = true;
            done = true;
        }
        sequence->isTerminal_ = result.isTerminal;
        currHistEntry->reward_ = result.reward;
        currHistEntry->action_ = result.action->copy();
        currHistEntry->transitionParameters_ = std::move(
                result.transitionParameters);
        currHistEntry->observation_ = result.observation->copy();

        // Add the next state to the pool
        StateInfo *nextStateInfo = allStates_->createOrGetInfo(*result.nextState);

        // Step forward in the history, and update the belief node.
        currentDiscount *= discountFactor;
        currHistEntry = currHistSeq->addEntry(nextStateInfo, currentDiscount);
        currNode = policy_->createOrGetChild(currNode, *result.action,
                *result.observation);
        currHistEntry->registerNode(currNode);

        if (rolloutUsed) {
            currHistEntry->totalDiscountedReward_ = qVal;
        } else {
            if (done) {
                //currHistEntry->immediateReward_ = model_->getReward(
                //            *nextStateInfo->getState());
                //currHistEntry->totalDiscountedReward_ = currHistEntry->discount_
                //    * currHistEntry->immediateReward_;
            }
        }
    }
    backup(currHistSeq);
    if (rolloutUsed) {
        updateHeuristicProbabilities(
                sequenceRoot->getBestMeanQValue() - initialRootQValue);
    }
    rolloutUsed = false;
}

void Solver::backup(HistorySequence *sequence) {
    std::vector<std::unique_ptr<HistoryEntry>>::reverse_iterator itHist = (
            sequence->histSeq_.rbegin());
    double totalReward;
    if ((*itHist)->action_ == nullptr) {
        totalReward = (*itHist)->totalDiscountedReward_;
    } else {
        totalReward = (*itHist)->totalDiscountedReward_ = (*itHist)->discount_
                * (*itHist)->reward_;
    }
    itHist++;
    for (; itHist != sequence->histSeq_.rend(); itHist++) {
        if ((*itHist)->hasBeenBackedUp_) {
            double previousTotalReward = (*itHist)->totalDiscountedReward_;
            totalReward = (*itHist)->totalDiscountedReward_ = (*itHist)->discount_
                    * (*itHist)->reward_ + totalReward;
            (*itHist)->owningBeliefNode_->updateQValue(
                    *(*itHist)->action_, totalReward - previousTotalReward);
        } else {
            totalReward = (*itHist)->totalDiscountedReward_ = (*itHist)->discount_
                    * (*itHist)->reward_ + totalReward;
            (*itHist)->owningBeliefNode_->updateQValue(
                    *(*itHist)->action_, totalReward, +1);
            (*itHist)->hasBeenBackedUp_ = true;
        }
    }
}

void Solver::undoBackup(HistorySequence *sequence) {
    std::vector<std::unique_ptr<HistoryEntry>>::reverse_iterator itHist =
            (sequence->histSeq_.rbegin());
    itHist++;
    for (; itHist != sequence->histSeq_.rend(); itHist++) {
        if ((*itHist)->hasBeenBackedUp_) {
            (*itHist)->owningBeliefNode_->updateQValue(
                    *(*itHist)->action_, -(*itHist)->totalDiscountedReward_, -1);
            (*itHist)->hasBeenBackedUp_ = false;
        } else {
            cerr << "ERROR: Backup not yet done; cannot undo!" << endl;
        }
    }
}

std::pair<Model::StepResult, double> Solver::getRolloutAction(
        BeliefNode *belNode, State const &state, double startDiscount,
        double discountFactor) {
    // We will try the next action that has not yet been tried.
    std::unique_ptr<Action> action = belNode->getNextActionToTry();
    Model::StepResult result = model_->generateStep(state, *action);
    double qVal = 0;

    if (std::bernoulli_distribution(
                heuristicProbability_[ROLLOUT_RANDHEURISTIC])(*randGen_)) {
        lastRolloutMode_ = ROLLOUT_RANDHEURISTIC;
    } else {
        lastRolloutMode_ = ROLLOUT_POL;
    }
    std::clock_t startTime, endTime;
    if (lastRolloutMode_ == ROLLOUT_POL) {
        startTime = std::clock();
        // Find a nearest neighbor as an approximation.
        BeliefNode *currNode = getNNBelNode(belNode);
        if (currNode == nullptr) {
            lastRolloutMode_ = ROLLOUT_RANDHEURISTIC;
            // Use RANDHEURISTIC instead.
        } else {
            currNode = currNode->getChild(*action, *result.observation);
            qVal = rolloutPolHelper(currNode, *result.nextState,
                        discountFactor);
            qVal *= startDiscount * discountFactor;
            lastRolloutMode_ = ROLLOUT_POL;
            endTime = std::clock();
        }
    }
    if (lastRolloutMode_ == ROLLOUT_RANDHEURISTIC) {
        startTime = std::clock();
        if (!result.isTerminal) {
            qVal = model_->getHeuristicValue(*result.nextState);
            qVal *= startDiscount * discountFactor;
        }
        lastRolloutMode_ = ROLLOUT_RANDHEURISTIC;
        endTime = std::clock();
    }
    timeUsedPerHeuristic_[lastRolloutMode_] +=
        (endTime - startTime) * 1000.0 / CLOCKS_PER_SEC;
    heuristicUseCount_[lastRolloutMode_]++;

    return std::make_pair(std::move(result), qVal);
}

double Solver::rolloutPolHelper(BeliefNode *currNode, State const &state,
        double discountFactor) {
    if (currNode == nullptr) {
        // cerr << "WARNING: nullptr in rolloutPolHelper!" << endl;
        return 0.0;
    } else if (currNode->getNParticles() == 0) {
        // cerr << "WARNING: nParticles == 0 in rolloutPolHelper" << endl;
        return 0.0;
    } else if (currNode->getNActChildren() == 0) {
        // cerr << "WARNING: No children in rolloutPolHelper" << endl;
        return 0.0;
    }

    std::unique_ptr<Action> action = currNode->getBestAction();
    Model::StepResult result = model_->generateStep(state, *action);
    currNode = currNode->getChild(*action, *result.observation);
    double qVal = result.reward;
    if (!result.isTerminal) {
        qVal += (discountFactor * rolloutPolHelper(
                         currNode, *result.nextState, discountFactor));
    }
    return qVal;
}

BeliefNode *Solver::getNNBelNode(BeliefNode *b) {
    double d, minDist;
    minDist = std::numeric_limits<double>::infinity();
    BeliefNode *nnBel = b->nnBel_;
    long numTried = 0;
    for (BeliefNode *node : policy_->allNodes_) {
        if (numTried >= model_->getMaxNnComparisons()) {
            break;
        } else {
            if (b->tNNComp_ < node->tLastAddedParticle_) {
                d = b->distL1Independent(node);
                if (d < minDist) {
                    minDist = d;
                    nnBel = node;
                }
            }
            numTried++;
        }
    }
    b->tNNComp_ = (double) (std::clock() - BeliefNode::startTime)
        * 1000 / CLOCKS_PER_SEC;
    b->nnBel_ = nnBel;
    if (minDist > model_->getMaxNnDistance()) {
        return nullptr;
    }
    return nnBel;
}

void Solver::updateHeuristicProbabilities(double valImprovement) {
    if (valImprovement < 0.0) {
        valImprovement = 0.0;
    }
    heuristicWeight_[lastRolloutMode_] *= std::exp(
                heuristicExploreCoefficient_
                * (valImprovement / model_->getMaxVal())
                / (2 * heuristicProbability_[lastRolloutMode_]));
    double totWRollout = 0.0;
    for (int i = 0; i < 2; i++) {
        totWRollout += heuristicWeight_[i];
    }
    double totP = 0.0;
    for (int i = 0; i < 2; i++) {
        heuristicProbability_[i] =
            ((1 - heuristicExploreCoefficient_) * heuristicWeight_[i]
             / totWRollout + heuristicExploreCoefficient_
             / 2) * heuristicUseCount_[i]
            / timeUsedPerHeuristic_[i];
        totP += heuristicProbability_[i];
    }
    for (int i = 0; i < 2; i++) {
        heuristicProbability_[i] /= totP;
    }
}

double Solver::runSim(long nSteps, std::vector<long> &changeTimes,
        std::vector<std::unique_ptr<State>> &trajSt,
        std::vector<std::unique_ptr<Action>> &trajAction,
        std::vector<std::unique_ptr<Observation>> &trajObs,
        std::vector<double> &trajRew, long *actualNSteps, double *totChTime,
        double *totImpTime) {
    trajSt.clear();
    trajAction.clear();
    trajObs.clear();
    trajRew.clear();

    *totChTime = 0.0;
    *totImpTime = 0.0;
    std::clock_t chTimeStart, chTimeEnd, impSolTimeStart, impSolTimeEnd;
    *actualNSteps = nSteps;
    long maxTrials = model_->getMaxTrials();
    long maximumDepth = model_->getMaximumDepth();
    double discFactor = model_->getDiscountFactor();
    double currDiscFactor = 1.0;
    double discountedTotalReward = 0.0;

    BeliefNode *currNode = policy_->getRoot();
    std::unique_ptr<State> state = model_->sampleAnInitState();
    // State *currentState = state.get();
    trajSt.push_back(state->copy());

    cout << "Initial State:" << endl;
    model_->drawState(*state, cout);

    std::vector<long>::iterator itCh = changeTimes.begin();
    for (long timeStep = 0; timeStep < nSteps; timeStep++) {
        cout << "t-" << timeStep << endl;
        allStates_->createOrGetInfo(*state);
        if (itCh != changeTimes.end() && timeStep == *itCh) {
            // Apply the changes to the model.
            cout << "Model changing." << endl;

            chTimeStart = std::clock();
            model_->update(*itCh, allStates_.get());
            if (changes::hasFlag(allStates_->getInfo(*state)->changeFlags_,
                    ChangeFlags::DELETED)) {
                cerr << "ERROR: Current simulation state deleted. Exiting.." << endl;
                std::exit(1);
            }
            for (std::unique_ptr<State> &state2 : trajSt) {
                if (changes::hasFlag(
                        allStates_->getInfo(*state2)->changeFlags_,
                        ChangeFlags::DELETED)) {
                    cerr << "ERROR: Impossible simulation history! Includes " << *state2 << endl;
                }
            }
            applyChanges();
            allStates_->resetAffectedStates();

            cout << "Changes complete" << endl;
            chTimeEnd = std::clock();

            *totChTime += ((chTimeEnd - chTimeStart) * 1000 / CLOCKS_PER_SEC);
            cout << "Total of " << *totChTime << " ms used for changes." << endl;

            itCh++;
        }
        impSolTimeStart = std::clock();
        improveSol(currNode, maxTrials, maximumDepth);
        impSolTimeEnd = std::clock();
        *totImpTime += ((impSolTimeEnd - impSolTimeStart) * 1000
                / CLOCKS_PER_SEC);

        Model::StepResult result = simAStep(currNode, *state);
        state = result.nextState->copy();

        trajAction.push_back(result.action->copy());
        trajObs.push_back(result.observation->copy());
        // trajSt is responsible for ownership
        trajSt.push_back(result.nextState->copy());
        trajRew.push_back(result.reward);
        discountedTotalReward += currDiscFactor * result.reward;
        currDiscFactor = currDiscFactor * discFactor;
        cout << "Discount: " << currDiscFactor << "; Total Reward: "
             << discountedTotalReward << endl;
        if (result.isTerminal) {
            *actualNSteps = timeStep;
            break;
        }

        BeliefNode *nextNode = currNode->getChild(*result.action,
                    *result.observation);
        if (nextNode == nullptr) {
            nextNode = addChild(currNode, *result.action, *result.observation,
                    timeStep);
        }
        currNode = nextNode;
    }
    return discountedTotalReward;
}

Model::StepResult Solver::simAStep(BeliefNode *currentBelief,
        State const &currentState) {
//    cout << "Belief node: ";
//    serializer_->save(*currentBelief, cout);

//    struct MyVisitor : public SpatialIndexVisitor {
//        MyVisitor(StatePool *statePool) :
//                    SpatialIndexVisitor(statePool),
//                    states() {
//        }
//        std::vector<StateInfo *> states;
//        void visit(StateInfo *info) {
//            states.push_back(info);
//        }
//    };
//    MyVisitor visitor(allStates_.get());
//    RTree *tree = static_cast<RTree *>(allStates_->getStateIndex());
//    if (model_->getName() == "Tag") {
//        clock_t startTime = std::clock();
//        for (int i = 0; i < 1000; i++) {
//            visitor.states.clear();
//            tree->boxQuery(visitor,
//                    std::vector<double> { 4,  0,  0,  0,  0,},
//                    std::vector<double> { 4,  0,  4,  9,  1,});
//        }
//        clock_t ticks = std::clock() - startTime;
//
//        cout << "Query results: " << endl;
//        for (StateInfo *info : visitor.states) {
//            cout << *info->getState() << endl;
//        }
//        cout << visitor.states.size() << " states; 1000 reps in " << (double)ticks / CLOCKS_PER_SEC << " seconds." << endl;
//    }

    State const *state = currentBelief->sampleAParticle(randGen_)->getState();
    cout << "Sampled particle: " << *state << endl;

    double totalDistance = 0;
    for (int i = 0; i < 100; i++) {
        State const *s1 = currentBelief->sampleAParticle(randGen_)->getState();
        State const *s2 = currentBelief->sampleAParticle(randGen_)->getState();
        totalDistance += s1->distanceTo(*s2);
    }
    cout << "Est. mean inter-particle distance: " << totalDistance / 100
         << endl;

    std::unique_ptr<Action> action = currentBelief->getBestAction();
    if (action == nullptr) {
        action = currentBelief->getNextActionToTry();
    }
    Model::StepResult result = model_->generateStep(currentState, *action);
    if (result.isTerminal) {
        cout << " Reached a terminal state." << endl;
    }
    cout << "Action: " << *result.action;
    cout << "; Reward: " << result.reward;
    cout << "; Obs: " << *result.observation << endl;
    model_->drawState(*result.nextState, cout);
    return result;
}

void Solver::improveSol(BeliefNode *startNode, long maxTrials,
        long maximumDepth) {
    if (startNode->getNParticles() == 0) {
        std::cerr << "ERROR: No particles in the BeliefNode!" << std::endl;
        std::exit(10);
    }
    double disc = model_->getDiscountFactor();
    std::vector<StateInfo *> samples;

    HistoryEntry *entry = startNode->particles_.get(0);
    long depth = entry->entryId_ + entry->owningSequence_->startDepth_;

    for (long i = 0; i < maxTrials; i++) {
        long index = std::uniform_int_distribution<long>(
                                         0, startNode->getNParticles() - 1)(*randGen_);
        // entry = startNode->sampleAParticle(randGen_);
        entry = startNode->particles_.get(index);
        bool found = false;
        for (std::unique_ptr<StateInfo> &s : allStates_->statesByIndex_) {
            if (s.get() == entry->stateInfo_) {
                found = true;
                break;
            }
        }
        if (!found) {
            cerr << "ERROR: INVALID STATE IN PARTICLE!" << endl;
            cerr << index << endl;
        }
        samples.push_back(entry->stateInfo_);
    }
    for (StateInfo *sample : samples) {
        singleSearch(startNode, sample, depth, disc, maximumDepth);
    }
}

BeliefNode *Solver::addChild(BeliefNode *currNode, Action const &action,
        Observation const &obs, long timeStep) {
    cerr << "WARNING: Adding particles due to depletion" << endl;
    BeliefNode *nextNode = policy_->createOrGetChild(currNode, action, obs);

    std::vector<State const *> particles;
    std::vector<HistoryEntry *>::iterator it;
    for (HistoryEntry *entry : currNode->particles_) {
        particles.push_back(entry->getState());
    }

    double discountFactor = model_->getDiscountFactor();
    double currentDiscount = std::pow(discountFactor, timeStep);
    // Attempt to generate particles for next state based on the current belief,
    // the observation, and the action.
    std::vector<std::unique_ptr<State>> nextParticles(
            model_->generateParticles(currNode, action, obs, particles));
    if (nextParticles.empty()) {
        cerr << "WARNING: Could not generate based on belief!" << endl;
        // If that fails, ignore the current belief.
        nextParticles = model_->generateParticles(currNode, action, obs);
    }
    if (nextParticles.empty()) {
        cerr << "ERROR: Failed to generate new particles!" << endl;
    }
    for (std::unique_ptr<State> &uniqueStatePtr : nextParticles) {
        StateInfo *stateInfo = allStates_->createOrGetInfo(*uniqueStatePtr);

        // Create a new history sequence and entry for the new particle.
        HistorySequence *histSeq = allHistories_->addNew(timeStep);
        HistoryEntry *histEntry = histSeq->addEntry(stateInfo, currentDiscount * discountFactor);
        histEntry->registerNode(nextNode);

//        State *state = stateInfo->getState();
//        if (!model_->isTerminal(*state)) {
//            histEntry->immediateReward_ = model_->getDefaultVal();
//            histEntry->totalDiscountedReward_ = (
//                    histEntry->discount_ * histEntry->immediateReward_);
//        }
        backup(histSeq);
    }
    return nextNode;
}

void Solver::applyChanges() {
    std::unordered_set<HistorySequence *> affectedSequences;
    for (StateInfo *stateInfo : allStates_->getAffectedStates()) {
        for (HistoryEntry *entry : stateInfo->usedInHistoryEntries_) {
            HistorySequence *sequence = entry->owningSequence_;
            long entryId = entry->entryId_;
            sequence->setChangeFlags(entryId, stateInfo->changeFlags_);
            if (changes::hasFlag(entry->changeFlags_, ChangeFlags::DELETED)) {
                if (entryId > 0) {
                    sequence->setChangeFlags(entryId - 1,
                            ChangeFlags::TRANSITION);
                }
            }
            if (changes::hasFlag(entry->changeFlags_,
                    ChangeFlags::OBSERVATION_BEFORE)) {
                if (entryId > 0) {
                    sequence->setChangeFlags(entryId - 1,
                            ChangeFlags::OBSERVATION);
                }
            }
            affectedSequences.insert(sequence);
        }
    }
    cout << "Updating " << affectedSequences.size() << " histories!" << endl;

    // Delete and remove any sequences where the first entry is now invalid.
    std::unordered_set<HistorySequence *>::iterator it = affectedSequences.begin();
    while (it != affectedSequences.end()) {
        HistorySequence *sequence = *it;
        undoBackup(sequence);
        if (changes::hasFlag(sequence->getEntry(0)->changeFlags_,
                ChangeFlags::DELETED)) {
            it = affectedSequences.erase(it);
            allHistories_->deleteHistorySequence(sequence->id_);
        } else {
            it++;
        }
    }

    // Revise all of the histories.
    historyCorrector_->reviseHistories(affectedSequences);

    // Clear flags and fix up all the sequences.
    for (HistorySequence *sequence : affectedSequences) {
        fixLinks(sequence);
        sequence->resetChangeFlags();
        if (sequence->isTerminal()) {
            backup(sequence);
        } else {
            continueSearch(sequence, model_->getDiscountFactor(),
                    model_->getMaximumDepth());
        }
    }
}

void Solver::fixLinks(HistorySequence *sequence) {
    if (sequence->invalidLinksStartId_ != -1) {
        std::vector<std::unique_ptr<HistoryEntry>>::iterator
        historyIterator = (sequence->histSeq_.begin()
                + sequence->invalidLinksStartId_);
        for ( ; (historyIterator + 1) != sequence->histSeq_.end();
                historyIterator++) {
            HistoryEntry *entry = historyIterator->get();
            HistoryEntry *nextEntry = (historyIterator + 1)->get();
            BeliefNode *nextNode = policy_->createOrGetChild(
                    entry->owningBeliefNode_, *entry->action_,
                    *entry->observation_);
            nextEntry->registerNode(nextNode);
        }
        sequence->invalidLinksStartId_ = -1;
    }
}
} /* namespace solver */
