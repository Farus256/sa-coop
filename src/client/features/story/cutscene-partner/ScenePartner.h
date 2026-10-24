#pragma once
// The partners' characters in the host's cutscenes: where each is in each shot and what it does there (up to three of
// them; with more partners the rest keep out of the scene). Every game places them itself near CJ, inside the
// picture, covering nobody, from the camera of the frame (ScenePartner.cpp says how).
#include <cstdint>
#include <string>
#include <vector>

#include "shared/game/Game.h"

namespace coop::scene {

// POSE_WALK: along with CJ, behind him to one side (act: 0 left, 1 right, 2 right behind), as his root track goes
// POSE_CAR: riding in a car of the scene, Spot::ground in the car's own frame, Spot::act the car's cutscene object
enum Pose : uint8_t { POSE_STAND, POSE_LEAN, POSE_SIT, POSE_STEP, POSE_HIDDEN, POSE_WALK, POSE_CAR };

struct Spot {
    game::CVector ground;  // where the feet are
    float heading = 0;     // radians
    Pose pose = POSE_STAND;
    uint8_t act = 0;       // which animation of the pose
};

// A cutscene is being loaded: the partner's animations and the cigarette are loaded with it (streaming is slow
// once the cutscene runs).
void Prepare();
// Looks at the frame being drawn: on a cut to another shot the spot is chosen again. Call it from the render
// hook while a cutscene runs: only then does the screen projection match the camera of the shot.
void Analyse();
// host: the partners in the scene, in the order they get places (call it before Analyse)
void SetCast(const std::vector<uint8_t>& ids);
// host, as a cutscene loads: whether the partners' characters take part in it at all. Off, every one keeps out of the
// whole cutscene (as "NAME hidden" in coop\cutscenes.txt) and the partners watch it without them.
void AllowExtras(bool on);
// host: a spot just chosen for player id and the moment (the camera's clock, ms) its shot begins, once per shot and
// player: call it until it says false
bool TakeDecision(uint8_t& id, Spot& s, float& at);
// partner: the host's choices; Follow (instead of Analyse) takes each on when this game's cutscene gets there
void FollowSpot(uint8_t id, const Spot& s, float at);
void Follow();
// a shot of a cutscene with CJ was taken on (spots chosen or followed)
bool HaveShot();
// Player id's spot for the current shot. False: none (no cutscene with CJ yet, or no place for that player).
bool CurrentSpot(uint8_t id, Spot& out);
void ForEachSpot(void (*fn)(uint8_t id, const Spot& s));
int ShotNumber();                     // grows with every new shot
float ShotLeftMs();                   // until the current shot's cut, by the camera's spline; -1 unknown
const char* Describe(const Spot& s);  // "lean GANGS/leanIDLE", for logs
// Keeps the ped on the spot with the spot's animation. Call every frame while the cutscene runs.
void Hold(game::CPed* ped, const Spot& s);
// The cutscene is over: animations, props and freezing off for every ped that was held.
void ReleaseAll();

// Review harness (tools/cs_map.py draws them): the last shot (camera, CJ, actors, every spot tried and why it
// was dropped) and the collision around CJ (buildings and objects, in world coordinates).
void DumpShot(const std::string& path);
void DumpWorld(const std::string& path);
game::CVector LastCj();

}  // namespace coop::scene
