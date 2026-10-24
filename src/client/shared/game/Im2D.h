#pragma once
// Flat shapes drawn by the game's RenderWare immediate mode (Im2D) in the middle of its own 2D drawing (the HUD, the
// radar, the map): triangles collected and sent in one draw call by Flush. Colours are ARGB.
#include <cstdint>

namespace coop::im2d {

constexpr uint32_t Argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | b;
}

void Quad(float x0, float y0, float x1, float y1, uint32_t c);
// a ring between two radii, its colour going from c0 at r0 to c1 at r1 (r0 = 0: a disc); n segments
void Ring(float cx, float cy, float r0, float r1, uint32_t c0, uint32_t c1, int n = 48);
void Flush();  // draws what was collected (no texture, alpha blended, no depth)

}  // namespace coop::im2d
