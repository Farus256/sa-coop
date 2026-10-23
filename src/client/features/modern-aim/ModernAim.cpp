// Aiming over the shoulder, as in the newer games. On foot with the mouse, a gun aimed in the third person
// (MODE_AIMWEAPON: pistols, SMGs, shotguns, rifles) put the camera far behind CJ with the crosshair off the centre
// (CCamera::m_f3rdPersonCHairMultX/Y 0.53, 0.4). Now the camera comes closer and over his right shoulder, the crosshair
// is a dot in the middle of the screen and the shots go there (CCamera::Find3rdPersonCamTargetVector aims through the
// crosshair's point), and the camera moves there and back smoothly. It never goes through a wall: the shift is cut
// short where the line from his head to it is blocked. The other players' games aim his copy with the camera and the
// crosshair he sends (Players), so they see the same shots. [game] modern_aim = 0 turns it off.
#include <algorithm>
#include <cmath>
#include <vector>

#include "common/lib/log/Log.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

constexpr uint16_t MODE_AIMWEAPON = 53;
// the shift over the shoulder (m): far enough behind that CJ stands whole in the left third of the picture
constexpr float kRight = 0.55f, kForward = 0.45f, kDown = -0.05f;
constexpr float kBlendPerSecond = 6.0f;                          // in and out in about a sixth of a second
constexpr int kFront = 0x190, kSource = 0x19C;                   // CCam::m_vecFront, m_vecSource
constexpr int kAlpha = 0xAC, kBeta = 0xBC;                       // CCam::m_fVerticalAngle, m_fHorizontalAngle
constexpr uintptr_t kFrameCounter = 0xB7CB4C;                    // CTimer::m_FrameCounter

float g_blend = 0.0f;      // 0: the game's camera, 1: over the shoulder
CVector g_applied{};       // what was added to the source last frame (taken off before the game works it out again)
void* g_appliedCam = nullptr;
uint32_t g_lastBlendMs = 0;
bool g_mults = false;      // our crosshair place is on
CVector g_followFront{0, 1, 0};  // where the following camera looked last
bool g_haveFollow = false;
uint32_t g_aimFrame = 0;         // the last frame the aiming camera ran
float g_multX = 0.53f, g_multY = 0.4f;

bool Enabled() { return Cfg().modernAim && cam::UseMouse3rdPerson(); }

void StepBlend(bool aiming) {
    uint32_t now = TimeMs();
    float dt = g_lastBlendMs ? std::min(0.1f, (now - g_lastBlendMs) / 1000.0f) : 0.0f;
    g_lastBlendMs = now;
    float target = aiming ? 1.0f : 0.0f;
    g_blend = g_blend < target ? std::min(target, g_blend + dt * kBlendPerSecond) : std::max(target, g_blend - dt * kBlendPerSecond);
}

float Ease(float t) { return t * t * (3.0f - 2.0f * t); }

void Shift(void* cam) {
    CVector& src = Field<CVector>(cam, kSource);
    if (g_blend <= 0.0f) {
        g_applied = {};
        return;
    }
    CPlayerPed* me = FindPlayerPed(0);
    if (!me) return;
    CVector front = Field<CVector>(cam, kFront).Normalized();
    CVector right = front.Cross(CVector{0, 0, 1}).Normalized();
    float k = Ease(g_blend);
    CVector want = right * kRight + front * kForward - CVector{0, 0, kDown};
    CVector head = me->GetPosition() + CVector{0, 0, 0.6f};
    // as far as nothing is in the way, from his head and from where the game put the camera
    float part = 1.0f;
    for (; part > 0.0f; part -= 0.25f) {
        CVector at = src + want * (k * part);
        if (Call<0x56A490, bool>(&head, &at, true, true, false, true, false, false, true) &&  // CWorld::GetIsLineOfSightClear
            Call<0x56A490, bool>(&src, &at, true, true, false, true, false, false, true))
            break;
    }
    g_applied = want * (k * std::max(0.0f, part));
    src += g_applied;
}

void Unshift(void* cam) {
    if (cam == g_appliedCam) Field<CVector>(cam, kSource) = Field<CVector>(cam, kSource) - g_applied;
    g_applied = {};
    g_appliedCam = cam;
}

void SetMults(bool on) {
    if (on == g_mults) return;
    if (on) {
        g_multX = cam::CrosshairMultX(), g_multY = cam::CrosshairMultY();
        cam::CrosshairMultX() = cam::CrosshairMultY() = 0.5f;
    } else {
        cam::CrosshairMultX() = g_multX, cam::CrosshairMultY() = g_multY;
    }
    g_mults = on;
}

using ProcessFn = void(__fastcall*)(void*, void*, const CVector*, float, float, float);
ProcessFn g_origAim = nullptr, g_origFollow = nullptr;

void __fastcall HookAimWeapon(void* cam, void* edx, const CVector* target, float a, float b, float c) {
    Unshift(cam);
    uint32_t frame = Ref<uint32_t>(kFrameCounter);
    bool entering = frame - g_aimFrame > 2;
    g_aimFrame = frame;
    g_origAim(cam, edx, target, a, b, c);
    bool on = Enabled() && Field<uint16_t>(cam, 0x0C) == MODE_AIMWEAPON;  // CCam::m_nMode
    // The aiming camera starts behind the way CJ faces (its first frame sets m_fHorizontalAngle from his heading): the
    // picture swung round whenever he faced elsewhere than the camera looked. It starts where the following camera
    // looked instead, CJ turns that way, and the frame is worked out again from there.
    if (on && entering && g_haveFollow) {
        CVector f = g_followFront.Normalized();
        float beta = std::atan2(-f.y, -f.x), alpha = std::asin(std::clamp(f.z, -0.99f, 0.99f));
        Field<float>(cam, kBeta) = beta;
        Field<float>(cam, kAlpha) = alpha;
        if (CPlayerPed* me = FindPlayerPed(0)) me->CurrentRotation() = me->AimingRotation() = WrapAngle(beta - 1.5707963f);
        g_origAim(cam, edx, target, a, b, c);
        Field<float>(cam, kBeta) = beta;
        Field<float>(cam, kAlpha) = alpha;
    }
    SetMults(on);
    StepBlend(on);
    if (on) Shift(cam);
}

// leaving the aim, the following camera starts where the aiming one was and moves back
void __fastcall HookFollowPed(void* cam, void* edx, const CVector* target, float a, float b, float c) {
    Unshift(cam);
    g_origFollow(cam, edx, target, a, b, c);
    g_followFront = Field<CVector>(cam, kFront);
    g_haveFollow = true;
    SetMults(false);
    StepBlend(false);
    if (Enabled()) Shift(cam);
}

// ---- drawing: one batch of triangles through RenderWare's immediate mode (one draw call for the whole scope)
struct Vtx {  // RwD3D9Vertex, the Im2D vertex
    float x, y, z, rhw;
    uint32_t color;
    float u, v;
};
std::vector<Vtx> g_tris;
constexpr uintptr_t kRwEngine = 0xC97B24;                       // RwEngineInstance (RwGlobals*)
constexpr uintptr_t kRenderStateSet = 0x20, kIm2DPrimitive = 0x30;  // RwGlobals::dOpenDevice (+0x10) functions
constexpr uintptr_t kNearScreenZ = 0xC4B8D8, kRecipNearClip = 0xC4B8D4;  // CSprite2d's
enum { RS_RASTER = 1, RS_ZTEST = 6, RS_ZWRITE = 8, RS_SRCBLEND = 10, RS_DESTBLEND = 11, RS_VERTEXALPHA = 12 };
enum { BLEND_SRCALPHA = 5, BLEND_INVSRCALPHA = 6, PRIM_TRILIST = 3 };

uint32_t Argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) { return (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | b; }
void V(float x, float y, uint32_t c) { g_tris.push_back({x, y, Ref<float>(kNearScreenZ), Ref<float>(kRecipNearClip), c, 0, 0}); }
void Quad(float x0, float y0, float x1, float y1, uint32_t c) {
    V(x0, y0, c), V(x1, y0, c), V(x1, y1, c);
    V(x0, y0, c), V(x1, y1, c), V(x0, y1, c);
}
// a ring between two radii, its colour fading from one edge to the other (a soft, smooth circle)
void Ring(float cx, float cy, float r0, float r1, uint32_t c0, uint32_t c1, int n = 160) {
    for (int i = 0; i < n; ++i) {
        float a0 = 6.2831853f * i / n, a1 = 6.2831853f * (i + 1) / n;
        float s0 = std::sin(a0), k0 = std::cos(a0), s1 = std::sin(a1), k1 = std::cos(a1);
        V(cx + k0 * r0, cy + s0 * r0, c0), V(cx + k1 * r0, cy + s1 * r0, c0), V(cx + k1 * r1, cy + s1 * r1, c1);
        V(cx + k0 * r0, cy + s0 * r0, c0), V(cx + k1 * r1, cy + s1 * r1, c1), V(cx + k0 * r1, cy + s0 * r1, c1);
    }
}
void Flush() {
    auto* rw = Ref<uint8_t*>(kRwEngine);
    if (!rw || g_tris.empty()) return;
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

// The sniper rifle's scope, drawn here instead of the game's: its texture is a 210x210 picture stretched to the
// screen's proportions (an oval on a wide screen) with a blurry edge. A round sight, sharp at any resolution: black
// round it with a soft inner edge, a slight vignette, a thin cross with heavy posts from the edge, mil dots, a red dot.
void DrawScope() {
    float w = (float)ScreenWidth(), h = (float)ScreenHeight();
    float cx = w * 0.5f, cy = h * 0.5f, r = h * 0.47f, px = std::max(1.0f, h / 1080.0f);
    uint32_t black = Argb(255, 0, 0, 0), clear = Argb(0, 0, 0, 0);
    float outerR = std::sqrt(w * w + h * h);
    Ring(cx, cy, r, outerR, black, black, 96);                   // all round the lens
    Ring(cx, cy, r - 2.5f * px, r, clear, black);               // its edge, smooth
    Ring(cx, cy, r * 0.72f, r - 2.5f * px, clear, Argb(110, 0, 0, 0));  // the lens darkens towards its rim
    uint32_t ink = Argb(235, 0, 0, 0);
    float thin = std::max(1.0f, 1.5f * px), thick = 5.0f * px, gap = r * 0.33f;
    Quad(cx - r, cy - thin * 0.5f, cx + r, cy + thin * 0.5f, ink);  // the thin cross
    Quad(cx - thin * 0.5f, cy - r, cx + thin * 0.5f, cy + r, ink);
    Quad(cx - r, cy - thick * 0.5f, cx - gap, cy + thick * 0.5f, ink);  // the posts from the edge
    Quad(cx + gap, cy - thick * 0.5f, cx + r, cy + thick * 0.5f, ink);
    Quad(cx - thick * 0.5f, cy + gap, cx + thick * 0.5f, cy + r, ink);
    for (int i = 1; i <= 4; ++i) {  // mil dots along the thin lines
        float d = gap * i / 5.0f, dot = 2.2f * px;
        for (float sx : {-1.0f, 1.0f}) Ring(cx + sx * d, cy, 0.0f, dot, ink, ink, 12);
        Ring(cx, cy + d, 0.0f, dot, ink, ink, 12);
    }
    Ring(cx, cy, 0.0f, 2.2f * px, Argb(255, 230, 30, 30), Argb(255, 230, 30, 30), 16);  // the dot where the shot goes
    Ring(cx, cy, 2.2f * px, 3.2f * px, Argb(255, 230, 30, 30), Argb(0, 230, 30, 30), 16);
    Flush();
}

// the crosshair of the third-person aim: a dot with a dark ring, where the shots go
using VoidFn = void(__cdecl*)();
VoidFn g_origCrossHairs = nullptr;
constexpr uint16_t MODE_SNIPER = 7;
constexpr int WEAPON_SNIPERRIFLE = 34;
void __cdecl HookCrossHairs() {
    CPlayerPed* me = FindPlayerPed(0);
    if (Cfg().betterScope && cam::Mode() == MODE_SNIPER && me && me->ActiveWeapon().type == WEAPON_SNIPERRIFLE) return DrawScope();
    if (!g_mults || cam::Mode() != MODE_AIMWEAPON) return g_origCrossHairs();
    float x = ScreenWidth() * cam::CrosshairMultX(), y = ScreenHeight() * cam::CrosshairMultY();
    float r = std::max(2.0f, std::floor(ScreenHeight() / 360.0f));
    Ring(x, y, r, r + std::max(1.5f, r * 0.6f), Argb(150, 0, 0, 0), Argb(0, 0, 0, 0), 24);
    Ring(x, y, 0.0f, r, Argb(235, 255, 255, 255), Argb(235, 255, 255, 255), 24);
    Ring(x, y, r, r + 1.0f, Argb(235, 255, 255, 255), Argb(0, 255, 255, 255), 24);
    Flush();
}

class ModernAim : public Module {
public:
    const char* Name() const override { return "ModernAim"; }
    void OnInstallHooks() override {
        mem::HookFn(0x521500, &HookAimWeapon, &g_origAim, "CCam::Process_AimWeapon");
        mem::HookFn(0x50F970, &HookFollowPed, &g_origFollow, "CCam::Process_FollowPedWithMouse");
        mem::HookFn(0x58E020, &HookCrossHairs, &g_origCrossHairs, "CHud::DrawCrossHairs");
        LOGI("Over-the-shoulder aiming: %s", Cfg().modernAim ? "on" : "off");
    }
    void OnGameShutdown() override {
        SetMults(false);
        g_blend = 0.0f;
        g_applied = {};
        g_appliedCam = nullptr;
    }
};

ModernAim g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

}  // namespace coop
