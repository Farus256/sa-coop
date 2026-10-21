#pragma once
// What a game makes for the players near it. Where players are together, one game makes the random people, traffic
// and trains for all of them (the others see its ones through the sync): the host's, else the lowest-numbered player's
// whose game makes its own. Each game tells the others whether it makes them (OnFootState::F_POPULATION, F_TRAINS);
// features/population and features/trains decide it.
#include <cstdint>

namespace coop {

struct WorldShare {
    bool population = true;  // this game makes its own people and traffic now
    bool trains = true;      // and its own random trains
};
WorldShare& Share();

// whether player `other` comes before this game's player in making what the players near each other share
bool MakesBefore(uint8_t other);

}  // namespace coop
