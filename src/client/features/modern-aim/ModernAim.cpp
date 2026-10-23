// The camera on foot over the shoulder, as in the newer games (with the mouse; [game] modern_aim = 0 turns it off). It
// stands behind CJ and to his right all the time; aiming a gun in the third person (MODE_AIMWEAPON: pistols, SMGs,
// shotguns, rifles) it only comes closer, the crosshair is a dot in the middle of the screen and the shots go there
// (CCamera::Find3rdPersonCamTargetVector aims through the crosshair's point). The game aimed with the camera far
// behind CJ and the crosshair off the centre (CCamera::m_f3rdPersonCHairMultX/Y 0.53, 0.4). The other players' games
// aim his copy with the camera and the crosshair he sends (Players), so they see the same shots.
#include <algorithm>
#include <cmath>
#include <vector>

#include "common/lib/log/Log.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

constexpr uint16_t MODE_AIMWEAPON = 53;
constexpr int kFront = 0x190, kSource = 0x19C, kFov = 0xB4;      // CCam::m_vecFront, m_vecSource, m_fFOV
constexpr int kAlpha = 0xAC, kBeta = 0xBC;                       // CCam::m_fVerticalAngle, m_fHorizontalAngle
constexpr uintptr_t kFrameCounter = 0xB7CB4C;                    // CTimer::m_FrameCounter

// One camera on foot, the way the newer games have it. Walking about it already stands behind CJ and over his right
// shoulder, looking past him the way the mouse turns it; aiming it only comes closer (and a little lower, the picture a
// little narrower), in a sixth of a second. The game's own two cameras, the following one and the aiming one, stood in
// different places with different heights and directions, and the cut between them jumped. The game still works out
// its cameras (the direction from the mouse, the aim, what the camera does in a car): only the place they stand at is
// ours, and each frame the game gets its own back before it goes on from it.
struct Rig {
    float dist, side, height, fov;
};
constexpr Rig kWalk{3.4f, 0.55f, 0.30f, 70.0f};   // walking about
constexpr Rig kAim{1.9f, 0.60f, 0.18f, 60.0f};    // aiming
constexpr float kBlendPerSecond = 6.0f;           // 0..1 in a sixth of a second

float g_blend = 0.0f;      // 0: walking, 1: aiming
uint32_t g_lastBlendMs = 0;
bool g_mults = false;      // our crosshair place is on
float g_multX = 0.53f, g_multY = 0.4f;
// the game's own camera of last frame, before ours (given back before it works the next one out)
void* g_ourCam = nullptr;
CVector g_gameSrc{}, g_gameFront{};
float g_gameFov = 70.0f;
bool g_changed = false;
// where each camera looked last: the other one starts looking there (the game's aiming camera started behind CJ's
// back, its following one where it had been before the aim)
CVector g_lookFront{0, 1, 0};
bool g_haveLook = false;
uint32_t g_aimFrame = 0, g_followFrame = 0;

bool Enabled() { return Cfg().modernAim && cam::UseMouse3rdPerson(); }
float Ease(float t) { return t * t * (3.0f - 2.0f * t); }

void StepBlend(bool aiming) {
    uint32_t now = TimeMs();
    float dt = g_lastBlendMs ? std::min(0.1f, (now - g_lastBlendMs) / 1000.0f) : 0.0f;
    g_lastBlendMs = now;
    float target = aiming ? 1.0f : 0.0f;
    g_blend = g_blend < target ? std::min(target, g_blend + dt * kBlendPerSecond) : std::max(target, g_blend - dt * kBlendPerSecond);
}

void GiveBack(void* cam) {
    if (cam == g_ourCam && g_changed) {
        Field<CVector>(cam, kSource) = g_gameSrc;
        Field<CVector>(cam, kFront) = g_gameFront;
        Field<float>(cam, kFov) = g_gameFov;
    }
    g_changed = false;
    g_ourCam = cam;
}

void KeepGame(void* cam) {
    g_gameSrc = Field<CVector>(cam, kSource);
    g_gameFront = Field<CVector>(cam, kFront);
    g_gameFov = Field<float>(cam, kFov);
    g_changed = true;
}

// the camera where the rig puts it: behind the pivot along the look, to the right, a little up; pulled in where a wall
// stands between it and CJ's head
void Place(void* cam) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || me->InVehicle()) return;
    float k = Ease(g_blend);
    Rig r{kWalk.dist + (kAim.dist - kWalk.dist) * k, kWalk.side + (kAim.side - kWalk.side) * k,
          kWalk.height + (kAim.height - kWalk.height) * k, kWalk.fov + (kAim.fov - kWalk.fov) * k};
    CVector front = Field<CVector>(cam, kFront).Normalized();
    CVector right = front.Cross(CVector{0, 0, 1}).Normalized();
    CVector pivot = me->GetPosition() + CVector{0, 0, 0.55f};
    CVector shoulder = pivot + right * r.side + CVector{0, 0, r.height};
    // the shoulder itself must be free (a wall at the right hand): the side shrinks first
    float side = r.side;
    for (int i = 0; i < 4; ++i) {
        CVector at = pivot + right * side + CVector{0, 0, r.height};
        if (Call<0x56A490, bool>(&pivot, &at, true, true, false, true, false, false, true)) {  // CWorld::GetIsLineOfSightClear
            shoulder = at;
            break;
        }
        side *= 0.5f;
        shoulder = pivot + CVector{0, 0, r.height};
    }
    float dist = r.dist;
    for (int i = 0; i < 6; ++i) {  // behind: as far as nothing is in the way
        CVector at = shoulder - front * dist;
        if (Call<0x56A490, bool>(&shoulder, &at, true, true, false, true, false, false, true)) break;
        dist *= 0.7f;
    }
    Field<CVector>(cam, kSource) = shoulder - front * std::max(dist, 0.6f);
    Field<float>(cam, kFov) = r.fov;
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

// the camera that starts now looks where the other one looked: its angles from that direction, and again
template <typename Fn>
void StartLooking(void* cam, Fn again, bool turnPlayer) {
    if (!g_haveLook) return;
    CVector f = g_lookFront.Normalized();
    float beta = std::atan2(-f.y, -f.x), alpha = std::asin(std::clamp(f.z, -0.99f, 0.99f));
    Field<float>(cam, kBeta) = beta;
    Field<float>(cam, kAlpha) = alpha;
    if (turnPlayer)
        if (CPlayerPed* me = FindPlayerPed(0)) me->CurrentRotation() = me->AimingRotation() = WrapAngle(beta - 1.5707963f);
    again();
    Field<float>(cam, kBeta) = beta;
    Field<float>(cam, kAlpha) = alpha;
}

using ProcessFn = void(__fastcall*)(void*, void*, const CVector*, float, float, float);
ProcessFn g_origAim = nullptr, g_origFollow = nullptr;

void __fastcall HookAimWeapon(void* cam, void* edx, const CVector* target, float a, float b, float c) {
    GiveBack(cam);
    uint32_t frame = Ref<uint32_t>(kFrameCounter);
    bool entering = frame - g_aimFrame > 2;
    g_aimFrame = frame;
    g_origAim(cam, edx, target, a, b, c);
    bool on = Enabled() && Field<uint16_t>(cam, 0x0C) == MODE_AIMWEAPON;  // CCam::m_nMode
    if (on && entering) StartLooking(cam, [&] { g_origAim(cam, edx, target, a, b, c); }, true);
    g_lookFront = Field<CVector>(cam, kFront);
    g_haveLook = true;
    SetMults(on);
    if (!on) return;
    KeepGame(cam);
    StepBlend(true);
    Place(cam);
}

// In a car, a passenger looks where it likes: aiming out of the window turns the camera with the mouse (the look button
// held, CPad::m_bVehicleMouseLook, or the aiming camera of MODE_AIMWEAPON_FROMCAR), and letting go the car's camera
// swung back behind the car, losing where the player had looked. For a passenger the car's camera stays turned where
// it was, relative to the car, and the mouse turns it on from there (as CCam's free mouse look does: 3 x the mouse's
// move x FOV/80 x the camera's mouse speed). The game's camera works out its frame and is turned round the car by
// what is between its angle and ours; the next frame it starts from ours.
using FollowCarFn = void(__fastcall*)(void*, void*, const CVector*, float, float, float, bool);
FollowCarFn g_origFollowCar = nullptr;
uint32_t g_carFrame = 0;
constexpr uintptr_t kMouseMoved = 0xB73418 + 0x0C;  // CPad::NewMouseControllerState.m_AmountMoved
constexpr uintptr_t kMouseAccelH = 0xB6EC1C;        // CCamera::m_fMouseAccelHorzntl
bool g_passLook = false;   // the passenger's own look holds
float g_passRel = 0;       // its angle from the car's heading
CVehicle* g_passCar = nullptr;

float CarAngle(CVehicle* v) {
    CMatrix* m = v->Matrix();
    return m ? std::atan2(m->forward.y, m->forward.x) : 0.0f;
}

void __fastcall HookFollowCar(void* cam, void* edx, const CVector* target, float a, float b, float c, bool d) {
    uint32_t frame = Ref<uint32_t>(kFrameCounter);
    bool afterAim = frame - g_carFrame > 2 && frame - g_aimFrame <= 3;
    g_carFrame = frame;
    g_origFollowCar(cam, edx, target, a, b, c, d);
    CPlayerPed* me = FindPlayerPed(0);
    CVehicle* v = me ? me->VehicleIfIn() : nullptr;
    if (!Net().Online() || !v || v->Driver() == me || !cam::UseMouse3rdPerson()) {
        g_passLook = false;
        return;
    }
    if (v != g_passCar) g_passLook = false, g_passCar = v;
    float& beta = Field<float>(cam, kBeta);
    bool held = GetPad(0)->NewState.m_bVehicleMouseLook;
    if (afterAim && g_haveLook) {  // from the aiming camera: where it looked
        CVector f = g_lookFront.Normalized();
        g_passRel = WrapAngle(std::atan2(-f.y, -f.x) - CarAngle(v));
        g_passLook = true;
    } else if (held) {  // turning with the look button: the game's turn is the passenger's
        g_passRel = WrapAngle(beta - CarAngle(v));
        g_passLook = true;
        return;
    }
    if (!g_passLook) return;
    float fov = Field<float>(cam, kFov) / 80.0f;
    g_passRel = WrapAngle(g_passRel - 3.0f * Ref<float>(kMouseMoved) * fov * Ref<float>(kMouseAccelH));
    float want = CarAngle(v) + g_passRel, turn = WrapAngle(want - beta);
    if (std::fabs(turn) < 1e-4f) return;
    // the frame turned round the car by the difference
    CVector pivot = v->GetPosition();
    CVector& src = Field<CVector>(cam, kSource);
    CVector& front = Field<CVector>(cam, kFront);
    float cs = std::cos(turn), sn = std::sin(turn);
    auto rot = [&](const CVector& p) { return CVector{p.x * cs - p.y * sn, p.x * sn + p.y * cs, p.z}; };
    src = pivot + rot(src - pivot);
    front = rot(front);
    beta = WrapAngle(want);
}

void __fastcall HookFollowPed(void* cam, void* edx, const CVector* target, float a, float b, float c) {
    GiveBack(cam);
    uint32_t frame = Ref<uint32_t>(kFrameCounter);
    bool entering = frame - g_followFrame > 2;
    g_followFrame = frame;
    g_origFollow(cam, edx, target, a, b, c);
    bool on = Enabled();
    if (on && entering) StartLooking(cam, [&] { g_origFollow(cam, edx, target, a, b, c); }, false);
    g_lookFront = Field<CVector>(cam, kFront);
    g_haveLook = true;
    SetMults(false);
    if (!on) return;
    KeepGame(cam);
    StepBlend(false);
    Place(cam);
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
        mem::HookFn(0x5245B0, &HookFollowCar, &g_origFollowCar, "CCam::Process_FollowCar_SA");
        mem::HookFn(0x58E020, &HookCrossHairs, &g_origCrossHairs, "CHud::DrawCrossHairs");
        LOGI("Over-the-shoulder aiming: %s", Cfg().modernAim ? "on" : "off");
    }
    void OnGameShutdown() override {
        SetMults(false);
        g_blend = 0.0f;
        g_changed = false;
        g_ourCam = nullptr;
        g_haveLook = false;
    }
};

ModernAim g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

}  // namespace coop
