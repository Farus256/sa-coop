#pragma once

namespace coop::patches {

// Applies engine patches required by co-op (and optional comfort patches from coop.ini):
//  * CGameLogic::IsCoopGameGoingOn always false (the native split-screen mode must never activate), and its 20 m leash
//    between the two players (IsPlayerAllowedToGoInThisDirection) always lets go
//  * run in background, no pause on focus loss, no auto pause menu, optional no-pause menu
//  * multiple game instances, windowed mode, custom user files directory, skip intro movies
void Apply();

// True while the local player is allowed to receive mouse look (false when our UI owns the mouse).
bool GameMouseEnabled();

// Windowed mode: restores our window style/size if the game or RW changed it (called each second).
void ApplyWindowPlacement();

// Fixes a stack bug in the inline hooks of plugins built with an old "injector" (they load until the game starts).
void FixPluginWrappers();

}  // namespace coop::patches
