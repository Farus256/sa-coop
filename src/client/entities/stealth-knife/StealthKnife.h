#pragma once
// People kill silently with a knife from behind, as the player can (the game's stealth kill, KNIFE.ifp): a ped of
// this game with a knife in hand, set on a player (a fight with it, CTaskComplexKillPedOnFoot, or marked as a
// knifeman by a battle of the mod), that gets right behind that player on foot while the player looks away, takes it
// by the head and cuts its throat: the ped plays KILL_Knife_Player, the player KILL_Knife_Ped_Die and dies. It says
// nothing meanwhile. The ped's game decides (its copies elsewhere only show it): for another player it tells that
// player's game (G_KNIFE), which kills its own player.
namespace game {
struct CPed;
}

namespace coop::knife {

void MarkKnifeman(game::CPed* ped);  // a battle's knifeman: goes for any player's back

}  // namespace coop::knife
