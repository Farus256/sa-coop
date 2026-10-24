// The sniper rifle's scope is drawn sharp and round ([game] better_scope = 0: the game's own). The cameras on foot and
// aiming out of a car's window are the game's own; a passenger's car camera stays where the player turned it.
// A first-person view, as the mods have it (F7, [keys] first_person, or /fp): the game's camera works as ever (the mouse
// turns it, it aims as it aims), but it looks from the player's eyes: after each camera's work (CCam::Process) its
// place is the head's (a little ahead of the face), the crosshair in the middle of the screen.
#include <algorithm>
#include <cmath>

#include "common/lib/util/Util.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "features/aim-view/AimView.h"
#include "shared/game/Game.h"
#include "shared/game/Im2D.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"

namespace coop {

namespace {
using namespace game;
using namespace im2d;

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

// ---- the first-person view
bool g_firstPerson = false;
float g_savedCrossX = 0.53f, g_savedCrossY = 0.4f;
bool FirstPersonMode(uint16_t mode) {
    switch (mode) {
    case 4: case 18: case 22: case 53: case 55: case 65:  // on foot, a car, a boat, aiming on foot / from a car / attached
        return true;
    }
    return false;
}

using CamProcessFn = void(__fastcall*)(void*, void*);
CamProcessFn g_origCamProcess = nullptr;
void __fastcall HookCamProcess(void* cam, void* edx) {
    g_origCamProcess(cam, edx);
    if (!g_firstPerson || (uintptr_t)cam != cam::Active() || cam::WideScreenOn() || !core::IsPlayerReady()) return;
    CPlayerPed* me = FindPlayerPed(0);
    uint16_t mode = Field<uint16_t>(cam, 0x0C);
    if (!me || me->Health() <= 0.0f || !FirstPersonMode(mode)) return;
    CVector head;
    CallMethod<0x5E4280>(me, &head, 5 /* BONE_HEAD */, true);  // CPed::GetBonePosition
    if (head.Length() < 1.0f) return;
    CVector& front = Field<CVector>(cam, kFront);
    CVector flat{front.x, front.y, 0.0f};
    float len = flat.Length();
    if (len > 0.01f) flat = flat * (1.0f / len);
    Field<CVector>(cam, kSource) = head + flat * 0.22f + CVector{0, 0, 0.06f};  // the eyes, ahead of the face
}

void SetFirstPerson(bool on) {
    if (on == g_firstPerson) return;
    g_firstPerson = on;
    if (on) {
        g_savedCrossX = cam::CrosshairMultX(), g_savedCrossY = cam::CrosshairMultY();
    } else {
        cam::CrosshairMultX() = g_savedCrossX, cam::CrosshairMultY() = g_savedCrossY;
    }
    ui::TheChat().AddSystem(on ? TR("Вид от первого лица (F7 — обратно).", "First-person view (F7: back).")
                               : TR("Вид от третьего лица.", "Third-person view."));
}

class AimView : public Module {
public:
    const char* Name() const override { return "AimView"; }
    void OnInstallHooks() override {
        mem::HookFn(0x58E020, &HookCrossHairs, &g_origCrossHairs, "CHud::DrawCrossHairs");
        mem::HookFn(0x5245B0, &HookFollowCar, &g_origFollowCar, "CCam::Process_FollowCar_SA");
        mem::HookFn(0x526FC0, &HookCamProcess, &g_origCamProcess, "CCam::Process");
    }
    void OnGameShutdown() override { g_holding = false, g_holdCar = nullptr; }
    void OnPostProcess() override {
        if (!core::IsPlayerReady()) return;
        FollowAim();
        if (g_firstPerson) cam::CrosshairMultX() = 0.5f, cam::CrosshairMultY() = 0.5f;  // the shot goes where the eyes look
    }
    bool OnCommand(const std::string& cmd, const std::vector<std::string>&) override {
        if (cmd != "fp" && cmd != "firstperson") return false;
        SetFirstPerson(!g_firstPerson);
        return true;
    }
};

AimView g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

namespace aimview {
bool KeyDown(int vk) {
    if (vk != Cfg().firstPersonKey || !core::IsPlayerReady() || MenuActive()) return false;
    SetFirstPerson(!g_firstPerson);
    return true;
}
}  // namespace aimview

}  // namespace coop
