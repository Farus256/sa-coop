#pragma once
// A partner who comes into the host's world for the first time (or into the host's new game) picks its character: CJ
// or one of the Grove Street Families, seen right there in the game (the camera before the player, the arrows switch,
// Enter takes it). The choice goes into its profile and comes back with it. A character who is not CJ cannot change
// clothes: the clothes shops, barbers and tattoo parlours stay shut for it, and the game never dresses it (CJ's clothes
// on another body crash the game).
#include <cstdint>
#include <string>

namespace coop::skin {

void Begin();                          // the choice as soon as the player can see itself
bool Active();                         // being chosen now
std::string Text(std::string& hint);   // the box in the middle of the screen while it is chosen
void Wear(uint16_t model);             // this game's player becomes that character (0: CJ in his clothes)
uint16_t Worn();                       // what this game's player is: 0 CJ, else the Families model

}  // namespace coop::skin
