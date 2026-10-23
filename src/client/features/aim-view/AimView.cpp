// What the camera and the sights do for a player, apart from the game's:
//  * the sniper rifle's scope is drawn sharp and round ([game] better_scope = 0: the game's own);
//  * a passenger in a car keeps looking where it aimed out of the window (online).
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

constexpr int kFront = 0x190, kSource = 0x19C, kFov = 0xB4;  // CCam::m_vecFront, m_vecSource, m_fFOV
constexpr int kBeta = 0xBC;                                  // CCam::m_fHorizontalAngle
constexpr uintptr_t kFrameCounter = 0xB7CB4C;                // CTimer::m_FrameCounter

// the aiming camera (on foot, from a car): where it looked last, and when (it is the game's own, untouched)
CVector g_lookFront{0, 1, 0};
bool g_haveLook = false;
uint32_t g_aimFrame = 0;

using ProcessFn = void(__fastcall*)(void*, void*, const CVector*, float, float, float);
ProcessFn g_origAim = nullptr;
void __fastcall HookAimWeapon(void* cam, void* edx, const CVector* target, float a, float b, float c) {
    g_origAim(cam, edx, target, a, b, c);
    g_aimFrame = Ref<uint32_t>(kFrameCounter);
    g_lookFront = Field<CVector>(cam, kFront);
    g_haveLook = true;
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

using VoidFn = void(__cdecl*)();
VoidFn g_origCrossHairs = nullptr;
constexpr uint16_t MODE_SNIPER = 7;
constexpr int WEAPON_SNIPERRIFLE = 34;
void __cdecl HookCrossHairs() {
    CPlayerPed* me = FindPlayerPed(0);
    if (Cfg().betterScope && cam::Mode() == MODE_SNIPER && me && me->ActiveWeapon().type == WEAPON_SNIPERRIFLE) return DrawScope();
    g_origCrossHairs();
}

class AimView : public Module {
public:
    const char* Name() const override { return "AimView"; }
    void OnInstallHooks() override {
        mem::HookFn(0x521500, &HookAimWeapon, &g_origAim, "CCam::Process_AimWeapon");
        mem::HookFn(0x5245B0, &HookFollowCar, &g_origFollowCar, "CCam::Process_FollowCar_SA");
        mem::HookFn(0x58E020, &HookCrossHairs, &g_origCrossHairs, "CHud::DrawCrossHairs");
    }
    void OnGameShutdown() override {
        g_haveLook = false;
        g_passLook = false;
    }
};

AimView g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

}  // namespace coop
