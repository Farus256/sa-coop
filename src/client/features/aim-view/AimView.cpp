// The sniper rifle's scope is drawn sharp and round ([game] better_scope = 0: the game's own). The cameras on foot and
// aiming out of a car's window are the game's own; a passenger's car camera stays where the player turned it.
// The first-person view is the first-person mod's (installed on its own, V); here it gets a walk back and keeps the
// aiming mod out of its way (below).
#include <windows.h>

#include <algorithm>
#include <cmath>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/game/Anim.h"
#include "shared/game/Im2D.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"

namespace coop {

namespace {
using namespace game;
using namespace im2d;
using game::script::Cmd;

// The sniper rifle's scope, drawn here instead of the game's: its texture is a 210x210 picture stretched to the
// screen's proportions (an oval on a wide screen) with a blurry edge. A round sight, sharp at any resolution: black
// round it with a soft inner edge, a slight vignette, a thin cross with heavy posts from the edge, mil dots, a red dot.
void DrawScope() {
    float w = (float)ScreenWidth(), h = (float)ScreenHeight();
    float cx = w * 0.5f, cy = h * 0.5f, r = h * 0.47f, px = std::max(1.0f, h / 1080.0f);
    uint32_t black = Argb(255, 0, 0, 0), clear = Argb(0, 0, 0, 0);
    float outerR = std::sqrt(w * w + h * h);
    Ring(cx, cy, r, outerR, black, black, 96);                   // all round the lens
    Ring(cx, cy, r - 2.5f * px, r, clear, black, 160);               // its edge, smooth
    Ring(cx, cy, r * 0.72f, r - 2.5f * px, clear, Argb(110, 0, 0, 0), 160);  // the lens darkens towards its rim
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

// ---- a passenger's camera
// The car's camera (CCam::Process_FollowCar_SA) swings back behind the car by itself a moment after the mouse rests: a
// passenger who had turned it to shoot out of the side window (the right button held turns it with the mouse) saw it
// go back behind the car the moment the button or the mouse let go, without having asked for it. For a passenger the
// game's own turn by the mouse (and by the look keys while they are held) still turns it, but nothing else does: it
// keeps its angle to the car (it turns with the car as it turns), and the frame the game worked out is turned round the
// car by whatever the game's own swing back added. 0.3.9 turned it by its own reckoning of the mouse instead, which did
// not feel as the game's camera does.
constexpr int kFront = 0x190, kSource = 0x19C, kBeta = 0xBC;  // CCam::m_vecFront, m_vecSource, m_fHorizontalAngle
constexpr uintptr_t kMouseMovedX = 0xB73418 + 0x0C;            // CPad::NewMouseControllerState.m_AmountMoved.x

using FollowCarFn = void(__fastcall*)(void*, void*, const CVector*, float, float, float, bool);
FollowCarFn g_origFollowCar = nullptr;
bool g_holding = false;   // the passenger's angle is kept
uint64_t g_aimEndedUntil = 0;  // an aim out of the window just ended: the car camera takes the aim's look as it is
float g_frontOffset = 0;  // the car camera's angle (Beta) less its look's (Front), to follow an aim's look
float g_rel = 0.0f;       // the camera's angle from the car's heading
CVehicle* g_holdCar = nullptr;

float Wrap(float a) {
    // in one step: subtracting 2pi in a loop never ends for a large angle (1e9, the "not set yet" heading of a held
    // cutscene partner, is a multiple of the float step there and froze the game in the cutscene)
    if (!std::isfinite(a)) return 0.0f;
    return std::remainder(a, 6.2831853f);
}
float CarAngle(CVehicle* v) {
    CMatrix* m = v->Matrix();
    return m ? std::atan2(m->forward.y, m->forward.x) : 0.0f;
}

void __fastcall HookFollowCar(void* cam, void* edx, const CVector* target, float a, float b, float c, bool d) {
    g_origFollowCar(cam, edx, target, a, b, c, d);
    CPlayerPed* me = FindPlayerPed(0);
    CVehicle* v = me ? me->VehicleIfIn() : nullptr;
    CEntity* follows = Field<CEntity*>(cam, 0x21C);  // CCam::m_pCamTargetEntity: this camera follows our car (or us in it)
    if (!v || v->Driver() == me || !cam::UseMouse3rdPerson() || cam::WideScreenOn() || (follows != (CEntity*)v && follows != (CEntity*)me)) {
        g_holding = false;
        return;
    }
    float& beta = Field<float>(cam, kBeta);
    if (v != g_holdCar || !g_holding) {  // just got in: as the game has it
        g_holdCar = v;
        g_holding = true;
        g_rel = Wrap(beta - CarAngle(v));
        return;
    }
    const CControllerState& pad = GetPad(0)->NewState;
    bool mouse = std::fabs(Ref<float>(kMouseMovedX)) > 0.0f;
    bool looking = pad.LeftShoulder2 || pad.RightShoulder2 || pad.ShockButtonR;  // looking aside or back: the game's
    if (mouse && NowMs() > g_aimEndedUntil) {  // the player turns it: the game's turn is the passenger's
        g_rel = Wrap(beta - CarAngle(v));
        return;
    }
    if (looking) return;
    g_frontOffset = Wrap(beta - std::atan2(Field<CVector>(cam, kFront).y, Field<CVector>(cam, kFront).x));
    float want = CarAngle(v) + g_rel, turn = Wrap(want - beta);
    if (std::fabs(turn) < 1e-4f) return;
    CVector pivot = v->GetPosition();
    CVector& src = Field<CVector>(cam, kSource);
    CVector& front = Field<CVector>(cam, kFront);
    float cs = std::cos(turn), sn = std::sin(turn);
    auto rot = [&](const CVector& p) { return CVector{p.x * cs - p.y * sn, p.x * sn + p.y * cs, p.z}; };
    src = pivot + rot(src - pivot);
    front = rot(front);
    beta = Wrap(want);
}

// Aiming out of the window the car camera rests (another camera, MODE_AIMWEAPON_FROMCAR, looks along the aim): the
// angle kept is the aim's, so that when the aim ends the car camera looks where the aim did - it went back to the
// angle from before the aim, behind the car.
constexpr uint16_t MODE_CAM_ON_A_STRING = 18;
// When the aim ends the camera does not fly anywhere: the game's blend from the aim's camera to the car's (CCamera::
// m_bTransitionState, +0x58) is cut, and the car camera starts where the aim looked (and stays there).
void FollowAim() {
    CPlayerPed* me = FindPlayerPed(0);
    CVehicle* v = me ? me->VehicleIfIn() : nullptr;
    static uint16_t lastMode = 0;
    uint16_t mode = cam::Mode();
    bool passenger = v && v->Driver() != me;
    if (passenger && mode == MODE_CAM_ON_A_STRING && lastMode != MODE_CAM_ON_A_STRING && lastMode != 0) {
        Ref<uint8_t>(addr::TheCamera + 0x58) = 0;  // CCamera::m_bTransitionState: no blend
        g_aimEndedUntil = NowMs() + 400;
    }
    lastMode = passenger ? mode : 0;
    if (!g_holding || !v || v != g_holdCar || v->Driver() == me || mode == MODE_CAM_ON_A_STRING) return;
    CVector f = cam::Front();
    if (std::fabs(f.x) + std::fabs(f.y) < 0.01f) return;
    g_rel = Wrap(std::atan2(f.y, f.x) + g_frontOffset - CarAngle(v));
}

// ---- with the first-person mod
// The first-person view is the mod of BoPoH (Voron295) with DK22Pac, DenSpb, Crspy and Junior_Djjr's fixes, installed
// on its own (V changes the camera to it, Alt+B its settings). Its view is seen here as the camera standing at the
// player's head (judged on foot without aiming and in a car's camera; while aiming it is what it was before).
//  * Backwards: in that view S turned CJ round to walk at the camera, and the camera at his head turned with him: one
//    could not walk back. Holding S there (not aiming) CJ keeps facing where the camera looks and steps back
//    (GunMove_BWD, the game's own walk back with a gun, from its root motion), unless the mod walks him back itself.
//  * The aiming mod (imCamAim.SA.asi, Advanced Aiming Mod) moves the aiming camera off the shoulder: in the
//    first-person view it pulled the camera out of the head and the two fought. Its call in CCam::Process (of
//    CCam::Process_AimWeapon, which it takes over) goes straight to the game's again while the first-person view is on,
//    and back to it in the third person.
bool g_fpView = false;
bool g_backing = false;
uint32_t g_bwdKey = 0;

CVector HeadOf(CPed* p) {
    CVector head;
    CallMethod<0x5E4280>(p, &head, 5 /* BONE_HEAD */, true);  // CPed::GetBonePosition
    return head;
}
CVector CameraPos() {  // TheCamera's own place (its matrix), whatever camera made it
    CMatrix* m = Field<CMatrix*>((void*)addr::TheCamera, 0x14);
    return m ? m->pos : Ref<CVector>(addr::TheCamera + 0x04);
}

void UpdateFpView() {
    CPlayerPed* me = FindPlayerPed(0);
    uint16_t mode = cam::Mode();
    if (!me || me->Health() <= 0.0f || cam::WideScreenOn()) {
        g_fpView = false;
        return;
    }
    if (mode != 4 /* FOLLOWPED */ && mode != 18 /* CAM_ON_A_STRING */) return;  // aiming and the rest: as it was
    CVector head = HeadOf(me);
    if (head.Length() < 1.0f) return;
    g_fpView = (CameraPos() - head).Length() < 0.7f;
}

bool ModWalksBack(CPed* me) {  // the mod's own strafing walk back is on
    if (!g_bwdKey) g_bwdKey = Call<0x53CF30, uint32_t>("GunMove_BWD");  // CKeyGen::GetUppercaseKey
    for (void* a = anim::First(me->RwObject()); a; a = anim::Next(a)) {
        void* hier = Field<void*>(a, 0x14);  // CAnimBlendAssociation::m_pHierarchy
        if (hier && Field<uint32_t>(hier, 0) == g_bwdKey) return true;  // CAnimBlendHierarchy::m_hashKey
    }
    return false;
}

void StopBacking(CPlayerPed* me) {
    if (!g_backing) return;
    g_backing = false;
    if (me) Cmd(0x0792).I(GetPedRef(me)).Run();  // CLEAR_CHAR_TASKS_IMMEDIATELY: the walk back off
}

void WalkBack() {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me) return;
    const CControllerState& pad = GetPad(0)->NewState;
    bool want = g_fpView && !me->InVehicle() && me->Health() > 0.0f && !MenuActive() && pad.LeftStickY > 60 && !pad.RightShoulder1 &&
                std::fabs(me->MoveSpeed().z) < 0.05f;  // on the ground
    if (!want) return StopBacking(me);
    CVector f = cam::Front();
    if (std::fabs(f.x) + std::fabs(f.y) < 0.01f) return;
    int h = GetPedRef(me);
    if (!g_backing) {
        if (ModWalksBack(me)) return;
        g_backing = true;
        Cmd(0x0605).I(h).S("GunMove_BWD").S("PED").F(4.0f).I(1).I(1).I(1).I(0).I(-1).Run();  // TASK_PLAY_ANIM: looped, moving
    }
    Cmd(0x0173).I(h).F(std::atan2(-f.x, f.y) * 57.29578f).Run();  // SET_CHAR_HEADING: facing where the camera looks
}

uintptr_t g_aimModCall = 0;  // the aiming mod's function at CCam::Process's call of Process_AimWeapon
bool g_aimModOff = false;
constexpr uintptr_t kAimWeaponCall = 0x527A95, kProcessAimWeapon = 0x521500;
void AimModFor(bool firstPerson) {
    static bool looked = false;
    if (!looked) {
        looked = true;
        uintptr_t t = mem::CallTarget(kAimWeaponCall);
        HMODULE mod = nullptr;
        wchar_t path[MAX_PATH] = {};
        if (t && t != kProcessAimWeapon &&
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)t, &mod) && mod &&
            GetModuleFileNameW(mod, path, MAX_PATH)) {
            std::wstring name = path;
            CharLowerBuffW(name.data(), (DWORD)name.size());
            if (name.find(L"camaim") != std::wstring::npos) {
                g_aimModCall = t;
                LOGI("The aiming mod is in (imCamAim): off in the first-person view");
            }
        }
    }
    if (!g_aimModCall || firstPerson == g_aimModOff) return;
    g_aimModOff = firstPerson;
    void* prev = nullptr;
    mem::RedirectCall(kAimWeaponCall, firstPerson ? (const void*)kProcessAimWeapon : (const void*)g_aimModCall, &prev);
}

class AimView : public Module {
public:
    const char* Name() const override { return "AimView"; }
    void OnInstallHooks() override {
        mem::HookFn(0x58E020, &HookCrossHairs, &g_origCrossHairs, "CHud::DrawCrossHairs");
        mem::HookFn(0x5245B0, &HookFollowCar, &g_origFollowCar, "CCam::Process_FollowCar_SA");
    }
    void OnGameShutdown() override {
        g_holding = false, g_holdCar = nullptr;
        g_backing = false, g_fpView = false;
    }
    void OnPostProcess() override {
        if (!core::IsPlayerReady()) return;
        FollowAim();
        UpdateFpView();
        WalkBack();
        AimModFor(g_fpView);
    }
};

AimView g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace


}  // namespace coop
