#pragma once
// Wounded, not dead: a player whose health runs out while another player is near (200 m) falls and lies where it fell
// for 30 seconds instead of dying. A partner who comes to it and presses the revive key (E) gives it CPR as the
// paramedics do (MEDIC, 5 seconds), and it gets up with every weapon it had and half its health. Each player can be
// revived twice in 6 minutes; after that it dies at once, as it does with nobody near, in a car or in the water. The
// wounded one can hold the key to give up. Its game decides all of it (the damage is its own): the others see it lie
// by its state (F_DOWNED, with the seconds left on its name tag) and the rescuer's CPR as its activity animation.
// Meanwhile nobody bothers it (everybody backs off, nobody targets it) and the host's mission goes on; only a player
// revives it, a rival gang's member within 30 m comes and kicks it (3 s off each kick).
#include <string>

namespace coop {

// the box in the middle of the screen: what is going on ("Вы ранены · 23 с") and the keys (the hint); empty: nothing
std::string ReviveText(std::string& hint);
// the revive key was pressed: true when it started or stopped a revive (the key does not go on to the game then)
bool ReviveKeyPressed();

}  // namespace coop
