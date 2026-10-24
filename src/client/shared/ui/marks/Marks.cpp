#include "shared/ui/marks/Marks.h"

#include <cmath>

#include "common/lib/util/Util.h"
#include "shared/config/Config.h"
#include "shared/ui/renderer/Renderer.h"

namespace coop::ui {

using namespace game;

void DrawWorldMarks(const std::vector<CVector>& marks, uint32_t argb, float lift, float size) {
    CPlayerPed* me = FindPlayerPed(0);
    if (marks.empty() || !me) return;
    auto& g = Gfx();
    float s = g.Scale() * size, W = g.Width(), H = g.Height();
    uint32_t text = (argb & 0xFF000000) | ((argb & 0x00FEFEFE) >> 1) | 0x00808080;  // a lighter one for the distance
    CVector camF = cam::Front(), camS = cam::Source();
    for (const CVector& at : marks) {
        CVector top = at + CVector{0, 0, lift};
        float dist = (at - me->GetPosition()).Length();
        float sx = 0, sy = 0, depth = 0;
        bool ahead = (top - camS).Dot(camF) > 0.5f;
        if (ahead && WorldToScreen(top, sx, sy, depth) && sx > 20 * s && sx < W - 20 * s && sy > 20 * s && sy < H - 20 * s) {
            float r = 10 * s;
            g.Poly4(sx, sy - r, sx + r * 0.7f, sy, sx, sy + r, sx - r * 0.7f, sy, argb);  // a diamond
            g.Text(FONT_SMALL, sx, sy + r + 2 * s, Format(TR("%.0f м", "%.0f m"), dist), text, TEXT_CENTER | TEXT_OUTLINE);
            continue;
        }
        // aside or behind: an arrow on the screen's edge, turned the way to it
        CVector d = at - camS;
        CVector right{camF.y, -camF.x, 0.0f};
        float x = d.Dot(right), y = d.Dot(CVector{camF.x, camF.y, 0.0f});
        float a = std::atan2(x, y);  // 0: ahead, +: to the right
        float cx = W * 0.5f + std::sin(a) * W * 0.42f, cy = H * 0.5f - std::cos(a) * H * 0.40f;
        float r = 14 * s;
        float ux = std::sin(a), uy = -std::cos(a), vx = -uy, vy = ux;  // the arrow points along (sin a, -cos a)
        g.Tri(cx + ux * r, cy + uy * r, cx - ux * r * 0.6f + vx * r * 0.7f, cy - uy * r * 0.6f + vy * r * 0.7f,
              cx - ux * r * 0.6f - vx * r * 0.7f, cy - uy * r * 0.6f - vy * r * 0.7f, argb);
        g.Text(FONT_SMALL, cx, cy + r, Format(TR("%.0f м", "%.0f m"), dist), text, TEXT_CENTER | TEXT_OUTLINE);
    }
}

}  // namespace coop::ui
