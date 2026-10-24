#pragma once
// The game's own 24/7s, bars and night clubs sell a little more: by the clerk, or at the counter by the barman, E
// opens a menu and 1-4 buy.
//  * a 24/7: a drink $50, food $25, a soda $150, cigarettes $250;
//  * a bar or a club: a drink $50, a snack $25.
// What each does is in Buffs. Every player buys for itself, in whichever game made the clerk: the clerk can be this
// game's own or the copy of another player's (the one who came in first brings the interior's people for all).
namespace coop::shops {

// a key while playing: true when the shops took it (it does not go on to the game then)
bool KeyDown(int vk);

}  // namespace coop::shops
