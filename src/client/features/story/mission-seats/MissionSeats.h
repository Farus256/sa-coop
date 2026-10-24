#pragma once
// Where the host's mission puts CJ, the partners near him go too.
//
// A mission script that warps CJ into a car (WARP_CHAR_INTO_CAR...) left the partners standing where they were: they
// walked to it, or were left behind when it drove off. A script that stands him on a vehicle (ATTACH_CHAR_TO_CAR: the
// back of a truck, a bike's pillion) left them no place on it at all. Now the host tells the
// partners near the vehicle (G_SEAT_CALL) where each of them goes: a free seat of that car; on a vehicle he stands on,
// a free seat of its cab first, then a place next to his on it, where they can shoot. What the mission's story asks of
// CJ (the boxes thrown from the truck) stays his. CJ getting into a car by himself calls nobody.
#include "shared/game/Game.h"

namespace coop::seats {

// host, around the commands of one of its mission's scripts (Missions' CRunningScript::Process)
void BeforeScript(game::CPlayerPed* cj);
void AfterScript(game::CPlayerPed* cj);

}  // namespace coop::seats
