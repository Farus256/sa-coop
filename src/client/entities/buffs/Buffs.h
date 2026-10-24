#pragma once
// What this game's player ate, drank and smoked (the goods of the 24/7s and the bars): effects that last a while.
//  * a stronger drink (20 min): one of steadier aim (the weapon skills up), quicker feet (walk and run faster) or more
//    health (the most health up), picked at random; and every 4 minutes a 32% chance of a minute drunk;
//  * food: health back to the full;
//  * a soda (30 min): more stamina, slightly quicker feet;
//  * cigarettes (30 min): steadier aim.
// The raised stats are the player's for the time only: a save or the profile sent to the server gets them without.
#include <cstdint>
#include <string>

namespace coop::buffs {

enum Item : uint8_t { DRINK, FOOD, SODA, CIGARETTES };

// the effect starts (or starts over); returns what to tell the player
std::string Use(Item item);
// the stats as they would be without the effects (the stats block and the most health of CPlayerInfo)
void Undo(uint8_t* statsBlock, uint8_t& maxHealth);
// the effects on now, as a short line for the screen ("Меткость 12:30 · Выносливость 25:10"); empty: none
std::string Summary();

}  // namespace coop::buffs
