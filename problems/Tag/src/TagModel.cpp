#include "TagModel.hpp"

#include <cmath>                        // for floor, pow
#include <cstddef>                      // for size_t

#include <fstream>                      // for ifstream, basic_istream, basic_istream<>::__istream_type
#include <iostream>                     // for cout, cerr
#include <map>                          // for map, _Rb_tree_iterator, map<>::iterator
#include <utility>                      // for pair

#include <boost/program_options.hpp>    // for variables_map, variable_value

#include "defs.hpp"                     // for RandomGenerator
#include "ChangeType.hpp"               // for ChangeType
#include "Observation.hpp"              // for Observation
#include "State.hpp"                    // for State

using std::cerr;
using std::cout;
using std::endl;
namespace po = boost::program_options;

TagModel::TagModel(RandomGenerator *randGen, po::variables_map vm) : Model(randGen) {
    // Read the map from the file.
    std::ifstream inFile;
    char const *mapPath = vm["problem.mapPath"].as<std::string>().c_str();
    inFile.open(mapPath);
    if (!inFile.is_open()) {
        std::cerr << "Fail to open " << mapPath << "\n";
        exit(1);
    }
    inFile >> nRows >> nCols;
    std::string tmp;
    getline(inFile, tmp);
    for (long i = 0; i < nRows; i++) {
        getline(inFile, tmp);
        mapText.push_back(tmp);
    }
    inFile.close();

    nParticles = vm["SBT.nParticles"].as<long>();
    maxTrials = vm["SBT.maxTrials"].as<long>();
    maxDistTry = vm["SBT.maxDistTry"].as<long>();

    exploreCoef = vm["SBT.exploreCoef"].as<double>();
    depthTh = vm["SBT.depthTh"].as<double>();
    distTh = vm["SBT.distTh"].as<double>();

    discount = vm["problem.discount"].as<double>();
    moveCost = vm["problem.moveCost"].as<double>();
    tagReward = vm["problem.tagReward"].as<double>();
    failedTagPenalty = vm["problem.failedTagPenalty"].as<double>();
    opponentStayProbability =
        vm["problem.opponentStayProbability"].as<double>();
    initialise();
    cout << "Constructed the TagModel" << endl;
    cout << "Discount: " << discount << endl;
    cout << "Size: " << nRows << " by " << nCols << endl;
    cout << "move cost: " << moveCost << endl;
    cout << "nActions: " << nActions << endl;
    cout << "nObservations: " << nObservations << endl;
    cout << "nStVars: " << nStVars << endl;
    VectorState s;
    cout << "Example States: " << endl;
    for (int i = 0; i < 5; i++) {
        sampleAnInitState(s);
        double q;
        solveHeuristic(s, &q);
        dispState(s, cout);
        cout << " Heuristic: " << q << endl;
    }
    cout << "nParticles: " << nParticles << endl;
    cout << "Environment:" << endl;
    drawEnv(cout);
}

void TagModel::initialise() {
    GridPosition p;
    nEmptyCells = 0;
    envMap.resize(nRows);
    for (p.i = nRows - 1; p.i >= 0; p.i--) {
        envMap[p.i].resize(nCols);
        for (p.j = 0; p.j < nCols; p.j++) {
            char c = mapText[p.i][p.j];
            long cellType;
            if (c == 'X') {
                cellType = WALL;
            } else {
                cellType = EMPTY + nEmptyCells;
                emptyCells.push_back(p);
                nEmptyCells++;
            }
            envMap[p.i][p.j] = cellType;
        }
    }

    nActions = 5;
    nObservations = nEmptyCells * 2;
    nStVars = 3;
    minVal = -failedTagPenalty / (1 - discount);
    maxVal = tagReward;
}

long TagModel::encodeGridPosition(GridPosition c) {
    return envMap[c.i][c.j];
}

GridPosition TagModel::decodeGridPosition(long code) {
    return emptyCells[code];
}

void TagModel::sampleAnInitState(VectorState &sVals) {
    sampleStateUniform(sVals);
}

void TagModel::sampleStateUniform(VectorState &sVals) {
    sVals.vals.resize(nStVars);
    sVals.vals[0] = global_resources::randIntBetween(0, nEmptyCells - 1);
    sVals.vals[1] = global_resources::randIntBetween(0, nEmptyCells - 1);
    sVals.vals[2] = UNTAGGED;
}

bool TagModel::isTerm(VectorState &sVals) {
    return sVals.vals[2] == TAGGED;
}

void TagModel::solveHeuristic(VectorState &s, double *qVal) {
    GridPosition robotPos = decodeGridPosition(s.vals[0]);
    GridPosition opponentPos = decodeGridPosition(s.vals[1]);
    if (s.vals[2] == TAGGED) {
        *qVal = 0;
        return;
    }
    int dist = robotPos.distance(opponentPos);
    double nSteps = dist / opponentStayProbability;
    double finalDiscount = std::pow(discount, nSteps);
    *qVal = -moveCost * (1 - finalDiscount) / (1 - discount);
    *qVal += finalDiscount * tagReward;
}

double TagModel::getDefaultVal() {
    return minVal;
}

bool TagModel::makeNextState(VectorState &sVals, unsigned long actId,
                             VectorState &nxtSVals) {
    nxtSVals = sVals;
    if (sVals.vals[2] == TAGGED) {
        return false;
    }
    GridPosition robotPos = decodeGridPosition(sVals.vals[0]);
    GridPosition opponentPos = decodeGridPosition(sVals.vals[1]);
    if (actId == TAG && robotPos == opponentPos) {
        nxtSVals.vals[2] = TAGGED;
        return true;
    }
    moveOpponent(robotPos, opponentPos);
    nxtSVals.vals[1] = encodeGridPosition(opponentPos);
    robotPos = getMovedPos(robotPos, actId);
    if (!isValid(robotPos)) {
        return false;
    }
    nxtSVals.vals[0] = encodeGridPosition(robotPos);
    return true;
}

void TagModel::makeOpponentActions(GridPosition &robotPos, GridPosition &opponentPos,
                                   std::vector<long> &actions) {
    if (robotPos.i > opponentPos.i) {
        actions.push_back(NORTH);
        actions.push_back(NORTH);
    } else if (robotPos.i < opponentPos.i) {
        actions.push_back(SOUTH);
        actions.push_back(SOUTH);
    } else {
        actions.push_back(NORTH);
        actions.push_back(SOUTH);
    }
    if (robotPos.j > opponentPos.j) {
        actions.push_back(WEST);
        actions.push_back(WEST);
    } else if (robotPos.j < opponentPos.j) {
        actions.push_back(EAST);
        actions.push_back(EAST);
    } else {
        actions.push_back(EAST);
        actions.push_back(WEST);
    }
}

void TagModel::moveOpponent(GridPosition &robotPos, GridPosition &opponentPos) {
    // Randomize to see if the opponent stays still.
    if (global_resources::rand01() < opponentStayProbability) {
        return;
    }
    std::vector<long> actions;
    makeOpponentActions(robotPos, opponentPos, actions);
    GridPosition newOpponentPos = getMovedPos(opponentPos,
                                        actions[global_resources::randIntBetween(0, actions.size() - 1)]);
    if (isValid(newOpponentPos)) {
        opponentPos = newOpponentPos;
    }
}

GridPosition TagModel::getMovedPos(GridPosition &GridPosition, unsigned long actId) {
    GridPosition movedPos = GridPosition;
    switch (actId) {
    case NORTH:
        movedPos.i -= 1;
        break;
    case EAST:
        movedPos.j += 1;
        break;
    case SOUTH:
        movedPos.i += 1;
        break;
    case WEST:
        movedPos.j -= 1;
    }
    return movedPos;
}

bool TagModel::isValid(GridPosition &GridPosition) {
    if (GridPosition.i < 0 || GridPosition.i >= nRows || GridPosition.j < 0 || GridPosition.j >= nCols
            || envMap[GridPosition.i][GridPosition.j] == WALL) {
        return false;
    }
    return true;
}

void TagModel::makeObs(VectorState &nxtSVals, unsigned long /*actId*/,
                       Observation &obsVals) {
    obsVals[0] = nxtSVals.vals[0];
    if (nxtSVals.vals[0] == nxtSVals.vals[1]) {
        obsVals[1] = SEEN;
    } else {
        obsVals[1] = UNSEEN;
    }
}

bool TagModel::getNextState(VectorState &sVals, unsigned long actId,
                            double *immediateRew, VectorState &nxtSVals, Observation &obs) {
    *immediateRew = getReward(sVals, actId);
    makeNextState(sVals, actId, nxtSVals);
    obs.resize(2);
    makeObs(nxtSVals, actId, obs);
    return isTerm(nxtSVals);
}

double TagModel::getReward(VectorState &/*sVals*/) {
    return 0;
}

double TagModel::getReward(VectorState &sVals, unsigned long actId) {
    if (actId == TAG) {
        if (sVals.vals[0] == sVals.vals[1]) {
            return tagReward;
        } else {
            return -failedTagPenalty;
        }
    } else {
        return -moveCost;
    }
}

void TagModel::getStatesSeeObs(unsigned long actId, Observation &obs,
                               std::vector<VectorState> &partSt, std::vector<VectorState> &partNxtSt) {
    std::map<std::vector<double>, double> weights;
    double weightTotal = 0;
    GridPosition newRobotPos = decodeGridPosition(obs[0]);
    if (obs[1] == SEEN) {
        VectorState nxtSVals;
        nxtSVals.vals.resize(nStVars);
        nxtSVals.vals[0] = nxtSVals.vals[1] = obs[0];
        nxtSVals.vals[2] = (actId == TAG ? TAGGED : UNTAGGED);
        partNxtSt.push_back(nxtSVals);
        return;
    }
    for (VectorState &sVals : partSt) {
        GridPosition oldRobotPos = decodeGridPosition(sVals.vals[0]);
        // Ignore states that do not match knowledge of the robot's position.
        if (newRobotPos != getMovedPos(oldRobotPos, actId)) {
            continue;
        }
        GridPosition oldOpponentPos = decodeGridPosition(sVals.vals[1]);
        std::vector<long> actions;
        makeOpponentActions(oldRobotPos, oldOpponentPos, actions);
        std::vector<long> newActions;
        for (long actionId : actions) {
            if (getMovedPos(oldOpponentPos, actionId) != newRobotPos) {
                newActions.push_back(actionId);
            }
        }
        double probabilityFactor = 1.0 / newActions.size();
        for (long action : newActions) {
            GridPosition newOpponentPos = getMovedPos(oldOpponentPos, action);
            VectorState sVals;
            sVals.vals.resize(nStVars);
            sVals.vals[0] = obs[0];
            sVals.vals[1] = encodeGridPosition(newOpponentPos);
            sVals.vals[2] = UNTAGGED;
            weights[sVals.vals] += probabilityFactor;
            weightTotal += probabilityFactor;
        }
    }
    double scale = nParticles / weightTotal;
    for (std::map<std::vector<double>, double>::iterator it = weights.begin();
            it != weights.end(); it++) {
        double proportion = it->second * scale;
        int numToAdd = std::floor(proportion);
        if (global_resources::rand01() <= (proportion - numToAdd)) {
            numToAdd += 1;
        }
        for (int i = 0; i < numToAdd; i++) {
            partNxtSt.emplace_back(it->first);
        }
    }
}

void TagModel::getStatesSeeObs(unsigned long actId, Observation &obs,
                               std::vector<VectorState> &partNxtSt) {
    if (obs[1] == SEEN) {
        VectorState nxtSVals;
        nxtSVals.vals.resize(nStVars);
        nxtSVals.vals[0] = nxtSVals.vals[1] = obs[0];
        nxtSVals.vals[2] = (actId == TAG ? TAGGED : UNTAGGED);
        partNxtSt.push_back(nxtSVals);
        return;
    }

    while (partNxtSt.size() < nParticles) {
        VectorState sVals;
        sampleStateUniform(sVals);
        VectorState nxtStVals;
        Observation obs2;
        double reward;
        getNextState(sVals, actId, &reward, nxtStVals, obs2);
        if (obs == obs2) {
            partNxtSt.push_back(nxtStVals);
        }
    }
}

void TagModel::getChangeTimes(char const */*chName*/,
                              std::vector<long> &/*chTime*/) {
}

void TagModel::update(long /*tCh*/, std::vector<VectorState> &/*affectedRange*/,
                      std::vector<ChangeType> &/*typeOfChanges*/) {
}

bool TagModel::modifStSeq(std::vector<VectorState> &/*seqStVals*/,
                          long /*startAffectedIdx*/, long /*endAffectedIdx*/,
                          std::vector<VectorState> &/*modifStSeq*/, std::vector<long> &/*modifActSeq*/,
                          std::vector<Observation> &/*modifObsSeq*/,
                          std::vector<double> &/*modifRewSeq*/) {
    return false;
}

void TagModel::dispAct(Action &action, std::ostream &os) {
    switch (action) {
    case NORTH:
        os << "NORTH";
        break;
    case EAST:
        os << "EAST";
        break;
    case SOUTH:
        os << "SOUTH";
        break;
    case WEST:
        os << "WEST";
        break;
    case TAG:
        os << "TAG";
        break;
    }
}

void dispObs(Observation const &obs, std::ostream &os) {
    os << decodeGridPosition(obs[0]);
    if (obs[1] == SEEN) {
        os << " SEEN!";
    }
}

void TagModel::dispCell(CellType cellType, std::ostream &os) {
        if (cellType >= EMPTY) {
            os << std::setw(2);
            os << cellType;
            return;
        }
        switch (cellType) {
        case WALL:
            os << "XX";
            break;
        default:
            os << "ERROR-" << cellType;
            break;
        }
    }

void TagModel::drawEnv(std::ostream &os) {
    for (std::vector<int> &row : envMap) {
        for (int cellType : row) {
            dispCell(cellType, os);
            os << " ";
        }
        os << endl;
    }
}

void TagModel::drawState(TagState &state, std::ostream &os) {
    for (std::size_t i = 0; i < envMap.size(); i++) {
        for (std::size_t j = 0; j < envMap[0].size(); j++) {
            GridPosition pos(i, j);
            bool hasRobot = (pos == state.getRobotPosition());
            bool hasOpponent = (pos == state.getOpponentPosition());
            if (hasRobot) {
                if (hasOpponent) {
                    os << "#";
                } else {
                    os << "r";
                }
            } else if (hasOpponent) {
                os << "o";
            } else {
                if (envMap[i][j] == WALL) {
                    os << "X";
                } else {
                    os << ".";
                }
            }
        }
        os << endl;
    }
}

