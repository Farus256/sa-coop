#include "shared/game/HudScale.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "common/lib/log/Log.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/memory/Memory.h"

namespace coop::hud {

namespace {
using namespace game;
constexpr uintptr_t kRwEngineInstance = 0xC97B24;  // RwGlobals*
// RwGlobals::dOpenDevice (+0x10): RwDevice::fpIm2DRenderPrimitive (+0x20), fpIm2DRenderIndexedPrimitive (+0x24)
constexpr uintptr_t kIm2DPrimitive = 0x30, kIm2DIndexed = 0x34;
constexpr uintptr_t kFontDetails = 0xC71A60;  // CFont::m_Details
// CFontDetails: Scale.x +4, SlopeRef.x +0x10, Centre +0x19, RightJustify +0x1A, WrapEnd +0x28, CentreX +0x2C,
// RightJustifyWrap +0x30
constexpr int kScaleX = 0x04, kSlopeRefX = 0x10, kWrapEnd = 0x28, kCentreX = 0x2C, kRightWrap = 0x30;

struct Vertex {  // RwD3D9Vertex, the Im2D vertex
    float x, y, z, rhw;
    uint32_t color;
    float u, v;
};

using PrimitiveFn = int(__cdecl*)(int, Vertex*, int);
using IndexedFn = int(__cdecl*)(int, Vertex*, int, uint16_t*, int);
PrimitiveFn g_origPrimitive = nullptr;
IndexedFn g_origIndexed = nullptr;

int g_depth = 0, g_suspend = 0, g_inFontRender = 0;
std::vector<Vertex> g_buf;

float ScreenW() { return (float)std::max(1, ScreenWidth()); }

// the point the picture keeps its place by: the edge of the screen it is near, or the centre
float AnchorFor(float x) {
    float w = ScreenW();
    if (x < w * 0.36f) return 0.0f;
    if (x > w * 0.64f) return w;
    return w * 0.5f;
}

float Narrow(float x, float anchor, float k) { return anchor + (x - anchor) * k; }

// Narrows a copy of the vertices: by quads for an indexed batch (the letters of a buffer can come from several strings,
// each quad keeps its own place), as a whole otherwise
Vertex* Transform(Vertex* v, int n, int group) {
    float k = Factor(), w = ScreenW();
    g_buf.assign(v, v + n);
    for (int i = 0; i < n; i += group) {
        int m = std::min(group, n - i);
        float lo = 1e9f, hi = -1e9f;
        for (int j = 0; j < m; ++j) lo = std::min(lo, g_buf[i + j].x), hi = std::max(hi, g_buf[i + j].x);
        if (hi - lo > w * 0.6f) continue;  // a fade, a black border: the whole screen as it is
        float a = AnchorFor((lo + hi) * 0.5f);
        for (int j = 0; j < m; ++j) g_buf[i + j].x = Narrow(g_buf[i + j].x, a, k);
    }
    return g_buf.data();
}

bool Narrowing() { return Active() && !g_inFontRender && Factor() < 0.99f; }

int __cdecl HookPrimitive(int type, Vertex* v, int n) {
    if (!Narrowing() || !v || n <= 0 || n > 4096) return g_origPrimitive(type, v, n);
    return g_origPrimitive(type, Transform(v, n, n), n);
}

int __cdecl HookIndexed(int type, Vertex* v, int n, uint16_t* idx, int ni) {
    if (!Narrowing() || !v || n <= 0 || n > 4096) return g_origIndexed(type, v, n, idx, ni);
    return g_origIndexed(type, Transform(v, n, 4), n, idx, ni);
}

// The device's functions are swapped in for the HUD's drawing only (another plugin may have swapped them too: theirs
// stays under ours)
void SwapIn() {
    auto* rw = Ref<uint8_t*>(kRwEngineInstance);
    if (!rw) return;
    auto& prim = *(PrimitiveFn*)(rw + kIm2DPrimitive);
    auto& indexed = *(IndexedFn*)(rw + kIm2DIndexed);
    if (prim != &HookPrimitive) g_origPrimitive = prim, prim = &HookPrimitive;
    if (indexed != &HookIndexed) g_origIndexed = indexed, indexed = &HookIndexed;
}

void SwapOut() {
    auto* rw = Ref<uint8_t*>(kRwEngineInstance);
    if (!rw) return;
    auto& prim = *(PrimitiveFn*)(rw + kIm2DPrimitive);
    auto& indexed = *(IndexedFn*)(rw + kIm2DIndexed);
    if (prim == &HookPrimitive && g_origPrimitive) prim = g_origPrimitive;
    if (indexed == &HookIndexed && g_origIndexed) indexed = g_origIndexed;
}

// ---- texts: narrowed where they are printed, by the point the alignment keeps (left, centre or right)
using PrintFn = void(__cdecl*)(float, float, const char*);
PrintFn g_origPrint = nullptr, g_origPrintBottom = nullptr;
using VoidFn = void(__cdecl*)();
VoidFn g_origRenderFontBuffer = nullptr;

template <PrintFn* orig>
void __cdecl HookPrint(float x, float y, const char* text) {
    if (!Active() || Factor() >= 0.99f) return (*orig)(x, y, text);
    uint8_t* d = (uint8_t*)kFontDetails;
    float k = Factor(), a = AnchorFor(x);
    float scale = *(float*)(d + kScaleX), slope = *(float*)(d + kSlopeRefX), wrap = *(float*)(d + kWrapEnd),
          centre = *(float*)(d + kCentreX), right = *(float*)(d + kRightWrap);
    *(float*)(d + kScaleX) = scale * k;
    *(float*)(d + kSlopeRefX) = Narrow(slope, a, k);
    *(float*)(d + kWrapEnd) = Narrow(wrap, a, k);
    *(float*)(d + kCentreX) = centre * k;  // a width
    *(float*)(d + kRightWrap) = Narrow(right, a, k);
    (*orig)(Narrow(x, a, k), y, text);
    *(float*)(d + kScaleX) = scale;
    *(float*)(d + kSlopeRefX) = slope;
    *(float*)(d + kWrapEnd) = wrap;
    *(float*)(d + kCentreX) = centre;
    *(float*)(d + kRightWrap) = right;
}

// the buffered letters were placed narrowed already: drawn as they are, whenever the buffer is flushed
void __cdecl HookRenderFontBuffer() {
    ++g_inFontRender;
    g_origRenderFontBuffer();
    --g_inFontRender;
}
}  // namespace

float Factor() {
    if (!Cfg().hudWidescreen) return 1.0f;
    float w = ScreenW(), h = (float)std::max(1, ScreenHeight());
    return std::min(1.0f, (4.0f / 3.0f) / (w / h));
}

bool Active() { return g_depth > 0 && g_suspend == 0; }

void Begin() {
    if (g_depth++ == 0 && Factor() < 0.99f) SwapIn();
}

void End() {
    if (g_depth > 0 && --g_depth == 0) SwapOut();
}

void Suspend() { ++g_suspend; }
void Resume() {
    if (g_suspend > 0) --g_suspend;
}

void Install() {
    mem::HookFn(0x71A700, &HookPrint<&g_origPrint>, &g_origPrint, "CFont::PrintString");
    mem::HookFn(0x71A820, &HookPrint<&g_origPrintBottom>, &g_origPrintBottom, "CFont::PrintStringFromBottom");
    mem::HookFn(0x719840, &HookRenderFontBuffer, &g_origRenderFontBuffer, "CFont::RenderFontBuffer");
    LOGI("HUD in its own proportions: %s", Cfg().hudWidescreen ? "on" : "off");
}

}  // namespace coop::hud
