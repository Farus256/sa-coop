#pragma once
// Marks on the screen over things in the world (the arena's last enemies, a tank battle's enemy tanks): a diamond
// over each with its distance, seen through the walls; off the screen (aside, behind) an arrow at the screen's edge
// turned the way to it.
#include <cstdint>
#include <vector>

#include "shared/game/Game.h"

namespace coop::ui {

// `lift`: how far over the point the mark stands; `size`: 1 for the usual mark
void DrawWorldMarks(const std::vector<game::CVector>& at, uint32_t argb, float lift = 1.3f, float size = 1.0f);

}  // namespace coop::ui
