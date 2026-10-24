#pragma once
// A battle of the mod (/arena, /tanks) for this game's player: no wanted stars while it lasts (the police leave the
// player alone, any star at once cleared), and what it took up there it does not keep: its weapons are noted when it
// begins, and when it ends each slot gets back what it had, with no more ammo than then (spent ammo stays spent).
// A weapon taken up in the battle goes; one lost there (a death in it) is not given back.
// Begin/End count: two battles at a time end when both have ended.
#include "shared/game/Types.h"

namespace game::challenge {

void Begin();
void End();
void Tick();   // every frame while one is on (the stars)
bool Active();

// A battle's pickups: every game makes the same ones itself, so while they are made they are not announced to the
// other games as this game's own (Pickups); one taken still goes for all.
void SetMakingPickups(bool on);
bool MakingPickups();

// A battle's grenade thrown by hand (CProjectileInfo::AddProjectile): the speed it leaves with, set on the projectile
// as soon as it is made and before Fire sends it to the other games (so that their copies fly the same arc).
void SetNextThrow(bool on, const CVector& from = {}, const CVector& speed = {});
bool NextThrow(CVector& from, CVector& speed);  // and clears it

}  // namespace game::challenge
