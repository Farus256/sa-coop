#include "shared/game/Im2D.h"

#include <cmath>
#include <vector>

#include "shared/game/Game.h"

namespace coop::im2d {

namespace {
using game::Ref;

struct Vtx {  // RwD3D9Vertex, the Im2D vertex
    float x, y, z, rhw;
    uint32_t color;
    float u, v;
};
std::vector<Vtx> g_tris;
constexpr uintptr_t kRwEngine = 0xC97B24;                            // RwEngineInstance (RwGlobals*)
constexpr uintptr_t kRenderStateSet = 0x20, kIm2DPrimitive = 0x30;   // RwGlobals::dOpenDevice (+0x10) functions
constexpr uintptr_t kNearScreenZ = 0xC4B8D8, kRecipNearClip = 0xC4B8D4;  // CSprite2d's
enum { RS_RASTER = 1, RS_ZTEST = 6, RS_ZWRITE = 8, RS_SRCBLEND = 10, RS_DESTBLEND = 11, RS_VERTEXALPHA = 12 };
enum { BLEND_SRCALPHA = 5, BLEND_INVSRCALPHA = 6, PRIM_TRILIST = 3 };

void V(float x, float y, uint32_t c) { g_tris.push_back({x, y, Ref<float>(kNearScreenZ), Ref<float>(kRecipNearClip), c, 0, 0}); }
}  // namespace

void Quad(float x0, float y0, float x1, float y1, uint32_t c) {
    V(x0, y0, c), V(x1, y0, c), V(x1, y1, c);
    V(x0, y0, c), V(x1, y1, c), V(x0, y1, c);
}

void Ring(float cx, float cy, float r0, float r1, uint32_t c0, uint32_t c1, int n) {
    for (int i = 0; i < n; ++i) {
        float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
        float s0 = std::sin(a0), k0 = std::cos(a0), s1 = std::sin(a1), k1 = std::cos(a1);
        V(cx + k0 * r0, cy + s0 * r0, c0), V(cx + k1 * r0, cy + s1 * r0, c0), V(cx + k1 * r1, cy + s1 * r1, c1);
        V(cx + k0 * r0, cy + s0 * r0, c0), V(cx + k1 * r1, cy + s1 * r1, c1), V(cx + k0 * r1, cy + s0 * r1, c1);
    }
}

void Flush() {
    auto* rw = Ref<uint8_t*>(kRwEngine);
    if (!rw || g_tris.empty()) {
        g_tris.clear();
        return;
    }
    using SetFn = int(__cdecl*)(int, void*);
    using PrimFn = int(__cdecl*)(int, Vtx*, int);
    auto set = *(SetFn*)(rw + kRenderStateSet);
    auto prim = *(PrimFn*)(rw + kIm2DPrimitive);
    set(RS_RASTER, nullptr);
    set(RS_VERTEXALPHA, (void*)1);
    set(RS_SRCBLEND, (void*)BLEND_SRCALPHA);
    set(RS_DESTBLEND, (void*)BLEND_INVSRCALPHA);
    set(RS_ZTEST, (void*)0);
    set(RS_ZWRITE, (void*)0);
    prim(PRIM_TRILIST, g_tris.data(), (int)g_tris.size());
    set(RS_ZWRITE, (void*)1);
    g_tris.clear();
}

}  // namespace coop::im2d
