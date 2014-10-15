/** @file tag/simulate.cpp
 *
 * Defines the main method for the "simulate" executable for the Tag POMDP, which runs online
 * simulations to test the performance of the solver.
 */
#include "problems/shared/simulate.hpp"

#include "PushBoxModel.hpp"
#include "ContNavOptions.hpp"

/** The main method for the "simulate" executable for Tag. */
int main(int argc, char const *argv[]) {
    return simulate<pushbox::PushBoxModel, pushbox::ContNavOptions>(argc, argv);
}
