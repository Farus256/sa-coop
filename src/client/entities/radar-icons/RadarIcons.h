#pragma once
// What the co-op draws on the radar and the pause menu's map besides the game's blips: the other players' waypoints,
// the places it adds to the city (a bar, a gambling den, a race). Drawn rather than added as script blips, so none of
// it ever gets into a save. Each icon is one of the game's radar sprites at a place in the world.
#include <cstdint>
#include <functional>

#include "shared/game/Game.h"

namespace coop::radar {

// an icon: shown on the radar while within its range, and on the map
void SetIcon(int key, const game::CVector& at, int sprite);
void RemoveIcon(int key);

// more drawing after the game's blips: (screen x, screen y) of world points comes from ToScreen
using Drawer = std::function<void()>;
void AddDrawer(Drawer d);
// a world point on the radar or the map being drawn; false: off the radar (clamped to its edge in `clamped` then)
bool ToScreen(const game::CVector2D& world, float& x, float& y, bool* clamped = nullptr);
bool DrawingMap();  // the pause menu's map is being drawn (not the radar)

}  // namespace coop::radar
