#include "Nav2DModel.hpp"

#define _USE_MATH_DEFINES
#include <cmath>                        // for pow, floor
#include <cstddef>                      // for size_t
#include <cstdlib>                      // for exit

#include <fstream>                      // for operator<<, basic_ostream, endl, basic_ostream<>::__ostream_type, ifstream, basic_ostream::operator<<, basic_istream, basic_istream<>::__istream_type
#include <initializer_list>
#include <iostream>                     // for cout, cerr
#include <memory>                       // for unique_ptr, default_delete
#include <random>                       // for uniform_int_distribution, bernoulli_distribution
#include <set>                          // for set, _Rb_tree_const_iterator, set<>::iterator
#include <string>                       // for string, getline, char_traits, basic_string
#include <tuple>                        // for tie, tuple
#include <unordered_map>                // for unordered_map<>::value_type, unordered_map
#include <utility>                      // for move, pair, make_pair
#include <vector>                       // for vector, vector<>::reference, __alloc_traits<>::value_type, operator==

#include <boost/program_options.hpp>    // for variables_map, variable_value

#include "global.hpp"                     // for RandomGenerator, make_unique
#include "problems/shared/geometry/Point2D.hpp"
#include "problems/shared/geometry/Vector2D.hpp"
#include "problems/shared/geometry/Rectangle2D.hpp"
#include "problems/shared/geometry/utilities.hpp"

#include "problems/shared/ModelWithProgramOptions.hpp"  // for ModelWithProgramOptions

#include "solver/geometry/Action.hpp"            // for Action
#include "solver/geometry/Observation.hpp"       // for Observation
#include "solver/geometry/State.hpp"       // for State

#include "solver/mappings/discretized_actions.hpp"
#include "solver/mappings/approximate_observations.hpp"

#include "solver/indexing/RTree.hpp"
#include "solver/indexing/FlaggingVisitor.hpp"

#include "solver/ChangeFlags.hpp"        // for ChangeFlags
#include "solver/Model.hpp"             // for Model::StepResult, Model
#include "solver/StatePool.hpp"

#include "Nav2DAction.hpp"         // for Nav2DAction
#include "Nav2DObservation.hpp"    // for Nav2DObservation
#include "Nav2DState.hpp"          // for Nav2DState

using std::cerr;
using std::cout;
using std::endl;

using geometry::Point2D;
using geometry::Vector2D;
using geometry::Rectangle2D;
using geometry::RTree;

namespace po = boost::program_options;

namespace nav2d {
Nav2DModel::Nav2DModel(RandomGenerator *randGen,
        po::variables_map vm) :
    ModelWithProgramOptions(randGen, vm),
    timeStepLength_(vm["problem.timeStepLength"].as<double>()),
    costPerUnitTime_(vm["problem.costPerUnitTime"].as<double>()),
    interpolationStepCount_(vm["problem.interpolationStepCount"].as<double>()),
    crashPenalty_(vm["problem.crashPenalty"].as<double>()),
    goalReward_(vm["problem.goalReward"].as<double>()),
    maxSpeed_(vm["problem.maxSpeed"].as<double>()),
    costPerUnitDistance_(vm["problem.costPerUnitDistance"].as<double>()),
    speedErrorType_(parseErrorType(
                vm["problem.speedErrorType"].as<std::string>())),
    speedErrorSD_(vm["problem.speedErrorSD"].as<double>()),
    maxRotationalSpeed_(vm["problem.maxRotationalSpeed"].as<double>()),
    costPerRevolution_(vm["problem.costPerRevolution"].as<double>()),
    rotationErrorType_(parseErrorType(
                vm["problem.rotationErrorType"].as<std::string>())),
    rotationErrorSD_(vm["problem.rotationErrorSD"].as<double>()),
    maxObservationDistance_(vm["SBT.maxObservationDistance"].as<double>()),
    nStVars_(2),
    minVal_(-(crashPenalty_ + maxSpeed_ * costPerUnitDistance_
            + maxRotationalSpeed_ * costPerRevolution_)
            / (1 - getDiscountFactor())),
    maxVal_(0),
    mapArea_(),
    startAreas_(),
    totalStartArea_(0),
    observationAreas_(),
    goalAreas_(),
    obstacles_(),
    obstacleTree_(nStVars_),
    goalAreaTree_(nStVars_),
    startAreaTree_(nStVars_),
    observationAreaTree_(nStVars_),
    changes_()
         {
    // Read the map from the file.
    std::ifstream inFile;
    char const *mapPath = vm["problem.mapPath"].as<std::string>().c_str();
    inFile.open(mapPath);
    if (!inFile.is_open()) {
        std::cerr << "Failed to open " << mapPath << endl;
        exit(1);
    }
    std::string line;
    while (std::getline(inFile, line)) {
        std::istringstream iss(line);
        std::string typeString;
        int64_t id;
        Rectangle2D rect;
        iss >> typeString >> id >> rect;
        AreaType areaType = parseAreaType(typeString);
        if (areaType == AreaType::WORLD) {
            mapArea_ = rect;
        } else {
            addArea(id, rect, areaType);
        }
    }
    inFile.close();

    cout << "Constructed the Nav2DModel" << endl;
    cout << "Discount: " << getDiscountFactor() << endl;
    cout << "nStVars: " << nStVars_ << endl;
    cout << "nParticles: " << getNParticles() << endl;
//    cout << "Testing random initial states:" << endl;
//    for (int i = 0; i < 2; i++) {
//        std::unique_ptr<solver::State> state = sampleAnInitState();
//        cout << *state << " ==> " << getHeuristicValue(*state) << endl;
//    }
//    cout << "Testing random states:" << endl;
//    for (int i = 0; i < 2; i++) {
//        std::unique_ptr<Nav2DState> state(
//                static_cast<Nav2DState *>(sampleStateUniform().release()));
//        cout << *state << " ==> " << getHeuristicValue(*state) << endl;
//        cout << getClosestPointOfType(state->getPosition(),
//                AreaType::GOAL) << endl;
//    }
//    cout << "Random state drawn:" << endl;
//    drawState(*sampleAnInitState(), cout);
}

std::string Nav2DModel::areaTypeToString(Nav2DModel::AreaType type) {
    switch(type) {
    case AreaType::EMPTY:
        return "Empty";
    case AreaType::WORLD:
        return "World";
    case AreaType::START:
        return "Start";
    case AreaType::OBSERVATION:
        return "Observation";
    case AreaType::GOAL:
        return "Goal";
    case AreaType::OBSTACLE:
        return "Obstacle";
    case AreaType::OUT_OF_BOUNDS:
        return "OOB";
    default:
        cerr << "ERROR: Invalid area code: " << static_cast<long>(type);
        return "ERROR";
     }
}

Nav2DModel::AreaType Nav2DModel::parseAreaType(std::string text) {
    if (text == "World") {
        return AreaType::WORLD;
    } else if (text == "Start") {
        return AreaType::START;
    } else if (text == "Observation") {
        return AreaType::OBSERVATION;
    } else if (text == "Goal") {
        return AreaType::GOAL;
    } else if (text == "Obstacle") {
        return AreaType::OBSTACLE;
    } else if (text == "Empty") {
        return AreaType::EMPTY;
    } else if (text == "OOB") {
        return AreaType::OUT_OF_BOUNDS;
    } else {
        cerr << "ERROR: Invalid area type: " << text;
        return AreaType::EMPTY;
    }
}

Nav2DModel::ErrorType Nav2DModel::parseErrorType(std::string text) {
    if (text == "proportional gaussian noise") {
        return ErrorType::PROPORTIONAL_GAUSSIAN_NOISE;
    } else if (text == "absolute gaussian noise") {
        return ErrorType::ABSOLUTE_GAUSSIAN_NOISE;
    } else if (text == "none") {
        return ErrorType::NONE;
    } else {
        cerr << "ERROR: Invalid error type - " << text;
        return ErrorType::PROPORTIONAL_GAUSSIAN_NOISE;
    }
}

double Nav2DModel::applySpeedError(double speed) {
    switch(speedErrorType_) {
    case ErrorType::PROPORTIONAL_GAUSSIAN_NOISE:
        speed = std::normal_distribution<double>(1.0, speedErrorSD_)(
                *getRandomGenerator()) * speed;
        if (speed < 0) {
            speed = 0;
        }
        return speed;
    case ErrorType::ABSOLUTE_GAUSSIAN_NOISE:
        speed = std::normal_distribution<double>(speed, speedErrorSD_)(
                *getRandomGenerator());
        if (speed < 0) {
            speed = 0;
        }
        return speed;
    case ErrorType::NONE:
        return speed;
    default:
        cerr << "Cannot calculate speed error";
        return speed;
    }
}

double Nav2DModel::applyRotationalError(double rotationalSpeed) {
    switch(rotationErrorType_) {
    case ErrorType::PROPORTIONAL_GAUSSIAN_NOISE:
        return rotationalSpeed * std::normal_distribution<double>(
                1.0, rotationErrorSD_)(*getRandomGenerator());
    case ErrorType::ABSOLUTE_GAUSSIAN_NOISE:
        return std::normal_distribution<double>(
                rotationalSpeed, speedErrorSD_)(*getRandomGenerator());
    case ErrorType::NONE:
        return rotationalSpeed;
    default:
        cerr << "Cannot calculate rotational error";
        return rotationalSpeed;
    }
}

void Nav2DModel::addArea(int64_t id, Rectangle2D const &area,
        Nav2DModel::AreaType type) {
    getAreas(type)->emplace(id, area);
    std::vector<double> lowCorner = area.getLowerLeft().asVector();
    std::vector<double> highCorner = area.getUpperRight().asVector();
    SpatialIndex::Region region(&lowCorner[0], &highCorner[0], nStVars_);
    getTree(type)->getTree()->insertData(0, nullptr, region, id);
    if (type == AreaType::START) {
        totalStartArea_ += area.getArea();
    }
}

std::unique_ptr<Nav2DState> Nav2DModel::sampleStateAt(Point2D position) {
    return std::make_unique<Nav2DState>(position,
            -std::uniform_real_distribution<double>(-0.5, 0.5)(
                    *getRandomGenerator()),
                    costPerUnitDistance_, costPerRevolution_);
}

std::unique_ptr<solver::State> Nav2DModel::sampleAnInitState() {
    RandomGenerator &randGen = *getRandomGenerator();
    double areaValue = std::uniform_real_distribution<double>(0,
            totalStartArea_)(randGen);
    double areaTotal = 0;
    for (AreasById::value_type const &entry : startAreas_) {
        areaTotal += entry.second.getArea();
        if (areaValue < areaTotal) {
            return std::make_unique<Nav2DState>(
                    entry.second.sampleUniform(randGen), 0,
                    costPerUnitDistance_, costPerRevolution_);
        }
    }
    cerr << "ERROR: Invalid area at " << areaValue << endl;
    return nullptr;
}

std::unique_ptr<solver::State> Nav2DModel::sampleStateUniform() {
    return sampleStateAt(mapArea_.sampleUniform(*getRandomGenerator()));
}

bool Nav2DModel::isTerminal(solver::State const &state) {
    return isInside(static_cast<Nav2DState const &>(state).getPosition(),
            AreaType::GOAL);
}

double Nav2DModel::getHeuristicValue(solver::State const &state) {
    Nav2DState const &navState = static_cast<Nav2DState const &>(state);
    Point2D closestPoint = getClosestPointOfType(navState.getPosition(),
            AreaType::GOAL);
    Vector2D displacement = closestPoint - navState.getPosition();
    double distance = displacement.getMagnitude();
    double turnAmount = std::abs(geometry::normalizeTurn(
            displacement.getDirection() - navState.getDirection()));
    double value = goalReward_;
    value -= costPerUnitDistance_ * distance;
    value -= costPerRevolution_ * turnAmount;
    value -= costPerUnitTime_ * distance / maxSpeed_;
    return value;
}

double Nav2DModel::getDefaultVal() {
    return minVal_;
}

std::unique_ptr<solver::TransitionParameters> Nav2DModel::generateTransition(
               solver::State const &state,
               solver::Action const &action) {
    Nav2DState const &navState = static_cast<Nav2DState const &>(state);
    Nav2DAction const &navAction = static_cast<Nav2DAction const &>(action);
    std::unique_ptr<Nav2DTransition> transition(
            std::make_unique<Nav2DTransition>());

    transition->speed = applySpeedError(navAction.getSpeed());
    transition->rotationalSpeed = applyRotationalError(
            navAction.getRotationalSpeed());
    Point2D position = navState.getPosition();
    double direction = navState.getDirection();
    double radius = transition->speed / (
            2 * M_PI * transition->rotationalSpeed);
    double turnAmount = transition->rotationalSpeed * timeStepLength_;
    Vector2D displacement(transition->speed * timeStepLength_, direction);

    transition->moveRatio = 0;
    Point2D center = position + Vector2D(radius, direction +
            turnAmount > 0 ? 0.25 : -0.25);

    for (long step = 1; step <= interpolationStepCount_; step++) {
        double previousRatio = transition->moveRatio;

        transition->moveRatio = (double)step / interpolationStepCount_;
        Point2D currentPosition;
        if (turnAmount == 0) {
            currentPosition = position + (transition->moveRatio
                    * displacement);
        } else {
            currentPosition = center + Vector2D(radius,
                    direction + transition->moveRatio * turnAmount +
                               turnAmount > 0 ? -0.25 : 0.25);
        }
        if (!mapArea_.contains(currentPosition)) {
            transition->moveRatio = previousRatio;
			break;
 		}
        if (isInside(currentPosition, AreaType::OBSTACLE)) {
            transition->moveRatio = previousRatio;
            transition->hadCollision = true;
            break;
        }
        if (isInside(currentPosition, AreaType::GOAL)){
            transition->reachedGoal = true;
            break;
        }
    }
    return std::move(transition);

}

std::unique_ptr<solver::State> Nav2DModel::generateNextState(
        solver::State const &state,
        solver::Action const &/*action*/,
        solver::TransitionParameters const *tp) {
    Nav2DState const &navState = static_cast<Nav2DState const &>(state);
    Point2D position = navState.getPosition();
    double direction = navState.getDirection();
    Nav2DTransition const &tp2 = static_cast<Nav2DTransition const &>(*tp);
    if (tp2.rotationalSpeed == 0.0) {
        position += Vector2D(tp2.moveRatio * tp2.speed * timeStepLength_,
                direction);
    } else {
        double radius = tp2.speed / (2 * M_PI * tp2.rotationalSpeed);
        Point2D center = position + Vector2D(radius,
                direction + tp2.rotationalSpeed > 0 ? 0.25 : -0.25);
        direction += tp2.moveRatio * tp2.rotationalSpeed * timeStepLength_;
        position = center + Vector2D(radius,
                direction + tp2.rotationalSpeed > 0 ? -0.25 : 0.25);
    }
    return std::make_unique<Nav2DState>(position, direction,
            costPerUnitDistance_,
            costPerRevolution_);
}


std::unique_ptr<solver::Observation> Nav2DModel::generateObservation(
        solver::State const */*state*/,
        solver::Action const &/*action*/,
        solver::TransitionParameters const */*tp*/,
        solver::State const &nextState) {
    Nav2DState const &navState = static_cast<Nav2DState const &>(nextState);
    if (isInside(navState.getPosition(), AreaType::OBSERVATION)) {
        return std::make_unique<Nav2DObservation>(navState);
    } else {
        return std::make_unique<Nav2DObservation>();
    }
}

double Nav2DModel::generateReward(
        solver::State const &/*state*/,
        solver::Action const &/*action*/,
        solver::TransitionParameters const *tp,
        solver::State const */*nextState*/) {
    Nav2DTransition const &tp2 = static_cast<Nav2DTransition const &>(*tp);
    double reward = 0;
    reward -= costPerUnitTime_ * timeStepLength_;
    double distance = tp2.moveRatio * tp2.speed * timeStepLength_;
    double turnAmount = tp2.moveRatio * tp2.rotationalSpeed * timeStepLength_;
    reward -= costPerUnitDistance_ * distance;
    reward -= costPerRevolution_ * turnAmount;
    if (tp2.reachedGoal) {
        reward += goalReward_;
    }
    if (tp2.hadCollision) {
        reward -= crashPenalty_;
    }
    return reward;
}

solver::Model::StepResult Nav2DModel::generateStep(
        solver::State const &state,
        solver::Action const &action) {
    solver::Model::StepResult result;
    result.action = action.copy();
    result.transitionParameters = generateTransition(state, action);
    result.nextState = generateNextState(state, action,
            result.transitionParameters.get());
    result.observation = generateObservation(nullptr, action,
            nullptr, *result.nextState);
    result.reward = generateReward(state, action,
            result.transitionParameters.get(), result.nextState.get());
	result.isTerminal = static_cast<Nav2DTransition const &>(
			*result.transitionParameters).reachedGoal;
    return result;
}

std::vector<long> Nav2DModel::loadChanges(char const *changeFilename) {
    std::vector<long> changeTimes;
       std::ifstream ifs;
       ifs.open(changeFilename);
       std::string line;
       while (std::getline(ifs, line)) {
           std::istringstream sstr(line);
           std::string tmpStr;
           long time;
           long nChanges;
           sstr >> tmpStr >> time >> tmpStr >> nChanges;

           changes_[time] = std::vector<Nav2DChange>();
           changeTimes.push_back(time);
           for (int i = 0; i < nChanges; i++) {
               std::getline(ifs, line);
               sstr.clear();
               sstr.str(line);

               Nav2DChange change;
               sstr >> change.operation;
               if (change.operation != "ADD") {
                   cerr << "ERROR: Cannot " << change.operation;
                   continue;
               }
               std::string typeString;
               sstr >> typeString;
               change.type = parseAreaType(typeString);
               sstr >> change.id;
               sstr >> change.area;
               changes_[time].push_back(change);
           }
       }
       ifs.close();
       return changeTimes;
}

void Nav2DModel::update(long time, solver::StatePool *pool) {
    for (Nav2DChange &change : changes_[time]) {
        cout << areaTypeToString(change.type) << " " << change.id;
        cout << " " << change.area << endl;
        addArea(change.id, change.area, change.type);
        solver::FlaggingVisitor visitor(pool, solver::ChangeFlags::DELETED);
        solver::RTree *tree = static_cast<solver::RTree *>(
                pool->getStateIndex());
        if (change.type == AreaType::OBSERVATION) {
            visitor.flagsToSet_ = solver::ChangeFlags::OBSERVATION_BEFORE;
        }
        tree->boxQuery(visitor,
                { change.area.getLowerLeft().getX(),
                        change.area.getLowerLeft().getY(), -2.0 },
                { change.area.getUpperRight().getX(),
                        change.area.getUpperRight().getY(), -2.0 });
    }
}

geometry::RTree *Nav2DModel::getTree(AreaType type) {
    switch(type) {
    case AreaType::GOAL:
        return &goalAreaTree_;
    case AreaType::OBSTACLE:
        return &obstacleTree_;
    case AreaType::START:
        return &startAreaTree_;
    case AreaType::OBSERVATION:
        return &observationAreaTree_;
    default:
        cerr << "ERROR: Cannot get tree; type " << static_cast<long>(type);
        cerr << endl;
        return nullptr;
    }
}

Nav2DModel::AreasById *Nav2DModel::getAreas(AreaType type) {
    switch(type) {
    case AreaType::GOAL:
        return &goalAreas_;
    case AreaType::OBSTACLE:
        return &obstacles_;
    case AreaType::START:
        return &startAreas_;
    case AreaType::OBSERVATION:
        return &observationAreas_;
    default:
        cerr << "ERROR: Cannot get area; type " << static_cast<long>(type);
        cerr << endl;
        return nullptr;
    }
}

bool Nav2DModel::isInside(geometry::Point2D point, AreaType type) {
    for (AreasById::value_type &entry : *getAreas(type)) {
        if (entry.second.contains(point)) {
            return true;
        }
    }
    return false;
    /*
    geometry::RTree *tree = getTree(type);
    SpatialIndex::Point p(&(point.asVector()[0]), nStVars_);
    class MyVisitor: public SpatialIndex::IVisitor {
    public:
        bool isInside = false;
        void visitNode(SpatialIndex::INode const &) {}
        void visitData(std::vector<SpatialIndex::IData const *> &) {}
        void visitData(SpatialIndex::IData const &) {
            isInside = true;
        }
    };
    MyVisitor v;
    tree->getTree()->pointLocationQuery(p, v);
    return v.isInside;
    */
}

Point2D Nav2DModel::getClosestPointOfType(Point2D point, AreaType type) {
    double infinity = std::numeric_limits<double>::infinity();
    double distance = infinity;
    Point2D closestPoint(infinity, infinity);
    for (AreasById::value_type &entry : *getAreas(type)) {
        Point2D newClosestPoint = entry.second.closestPointTo(point);
        double newDistance = (point - newClosestPoint).getMagnitude();
        if (newDistance < distance) {
            distance = newDistance;
            closestPoint = newClosestPoint;
        }
    }
    return closestPoint;
}

double Nav2DModel::getDistance(Point2D point, AreaType type) {
    double distance = std::numeric_limits<double>::infinity();
    for (AreasById::value_type &entry : *getAreas(type)) {
        double newDistance = entry.second.distanceTo(point);
        if (newDistance < distance) {
            distance = newDistance;
        }
    }
    return distance;
    /*
    geometry::RTree *tree = getTree(type);
    SpatialIndex::Point p(&(point.asVector()[0]), nStVars_);
    class MyVisitor: public SpatialIndex::IVisitor {
    public:
        SpatialIndex::Point &p_;
        double distance_;
        MyVisitor(SpatialIndex::Point &p) :
                p_(p), distance_(std::numeric_limits<double>::infinity()) {}
        void visitNode(SpatialIndex::INode const &) {}
        void visitData(std::vector<SpatialIndex::IData const *> &) {
        }
        void visitData(SpatialIndex::IData const &data) {
            SpatialIndex::IShape *shape;
            data.getShape(&shape);
            distance_ = shape->getMinimumDistance(p_);
        }
    };
    MyVisitor v(p);
    tree->getTree()->nearestNeighborQuery(1, p, v);
    return v.distance_;
    */
}

Nav2DModel::AreaType Nav2DModel::getAreaType(geometry::Point2D point) {
    if (!mapArea_.contains(point)) {
        return AreaType::OUT_OF_BOUNDS;
    } else if (isInside(point, AreaType::OBSTACLE)) {
        return AreaType::OBSTACLE;
    } else if (isInside(point, AreaType::GOAL)) {
        return AreaType::GOAL;
    } else if (isInside(point, AreaType::START)) {
        return AreaType::START;
    } else if (isInside(point, AreaType::OBSERVATION)) {
        return AreaType::OBSERVATION;
    } else {
        return AreaType::EMPTY;
    }
}

void Nav2DModel::dispPoint(Nav2DModel::AreaType type, std::ostream &os) {
    switch(type) {
    case AreaType::EMPTY:
        os << " ";
        return;
    case AreaType::START:
        os << "+";
        return;
    case AreaType::GOAL:
        os << "*";
        return;
    case AreaType::OBSTACLE:
        os << "%";
        return;
    case AreaType::OBSERVATION:
        os << "x";
        return;
    case AreaType::OUT_OF_BOUNDS:
        os << "#";
        return;
    default:
        cerr << "ERROR: Invalid point type!?" << endl;
        return;
    }
}

void Nav2DModel::drawEnv(std::ostream &os) {
    double minX = mapArea_.getLowerLeft().getX();
    double maxX = mapArea_.getUpperRight().getX();
    double minY = mapArea_.getLowerLeft().getY();
    double maxY = mapArea_.getUpperRight().getY();
    double height = maxY - minY;
    long nRows = 30; //(int)height;
    double width = maxX - minX;
    long nCols = (int)width;
    for (long i = 0; i <= nRows + 1; i++) {
        double y = (nRows + 0.5 - i) * height / nRows;
        for (long j = 0; j <= nCols + 1; j++) {
            double x = (j - 0.5) * width / nCols;
            dispPoint(getAreaType({x, y}), os);
        }
        os << endl;
    }
}

void Nav2DModel::drawState(solver::State const &state, std::ostream &os) {
    Nav2DState const &navState = static_cast<Nav2DState const &>(state);
    double minX = mapArea_.getLowerLeft().getX();
    double maxX = mapArea_.getUpperRight().getX();
    double minY = mapArea_.getLowerLeft().getY();
    double maxY = mapArea_.getUpperRight().getY();
    double height = maxY - minY;
    long nRows = 30; //(int)height;
    double width = maxX - minX;
    long nCols = (int)width;

    long stateI = nRows - (int)std::round(navState.getY() * nRows / height - 0.5);
    long stateJ = (int)std::round(navState.getX() * nCols / width + 0.5);
    for (long i = 0; i <= nRows + 1; i++) {
        double y = (nRows + 0.5 - i) * height / nRows;
        for (long j = 0; j <= nCols + 1; j++) {
            double x = (j - 0.5) * width / nCols;
            if (i == stateI && j == stateJ) {
                os << "o";
            } else {
                dispPoint(getAreaType({x, y}), os);
            }
        }
        os << endl;
    }
    os << state << endl;
}

long Nav2DModel::getNumberOfBins() {
    return static_cast<long>(ActionType::END);
}

std::unique_ptr<solver::EnumeratedPoint> Nav2DModel::sampleAnAction(
        long code) {
    return std::make_unique<Nav2DAction>(static_cast<ActionType>(code), this);
}

double Nav2DModel::getMaxObservationDistance() {
    return maxObservationDistance_;
}
} /* namespace nav2d */