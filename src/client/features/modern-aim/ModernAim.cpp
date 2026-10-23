// Aiming over the shoulder, as in the newer games. On foot with the mouse, a gun aimed in the third person
// (MODE_AIMWEAPON: pistols, SMGs, shotguns, rifles) put the camera far behind CJ with the crosshair off the centre
// (CCamera::m_f3rdPersonCHairMultX/Y 0.53, 0.4). Now the camera comes closer and over his right shoulder, the crosshair
// is a dot in the middle of the screen and the shots go there (CCamera::Find3rdPersonCamTargetVector aims through the
// crosshair's point), and the camera moves there and back smoothly. It never goes through a wall: the shift is cut
// short where the line from his head to it is blocked. The other players' games aim his copy with the camera and the
// crosshair he sends (Players), so they see the same shots. [game] modern_aim = 0 turns it off.
#include <algorithm>
#include <cmath>

#include "common/lib/log/Log.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

constexpr uint16_t MODE_AIMWEAPON = 53;
constexpr float kRight = 0.5f, kForward = 1.1f, kDown = 0.12f;  // the shift over the shoulder (m)
constexpr float kBlendPerSecond = 6.0f;                          // in and out in about a sixth of a second
constexpr int kFront = 0x190, kSource = 0x19C;                   // CCam::m_vecFront, m_vecSource

float g_blend = 0.0f;      // 0: the game's camera, 1: over the shoulder
CVector g_applied{};       // what was added to the source last frame (taken off before the game works it out again)
void* g_appliedCam = nullptr;
uint32_t g_lastBlendMs = 0;
bool g_mults = false;      // our crosshair place is on
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
    g_origAim(cam, edx, target, a, b, c);
    bool on = Enabled() && Field<uint16_t>(cam, 0x0C) == MODE_AIMWEAPON;  // CCam::m_nMode
    SetMults(on);
    StepBlend(on);
    if (on) Shift(cam);
}

// leaving the aim, the following camera starts where the aiming one was and moves back
void __fastcall HookFollowPed(void* cam, void* edx, const CVector* target, float a, float b, float c) {
    Unshift(cam);
    g_origFollow(cam, edx, target, a, b, c);
    SetMults(false);
    StepBlend(false);
    if (Enabled()) Shift(cam);
}

// the crosshair of the third-person aim: a dot with a dark ring, where the shots go
using VoidFn = void(__cdecl*)();
VoidFn g_origCrossHairs = nullptr;
void __cdecl HookCrossHairs() {
    if (!g_mults || cam::Mode() != MODE_AIMWEAPON) return g_origCrossHairs();
    float x = ScreenWidth() * cam::CrosshairMultX(), y = ScreenHeight() * cam::CrosshairMultY();
    float r = std::max(2.0f, std::floor(ScreenHeight() / 360.0f)), o = r + std::max(1.0f, r * 0.5f);
    CRGBA dark{0, 0, 0, 150}, light{255, 255, 255, 230};
    float outer[4] = {x - o, y + o, x + o, y - o}, inner[4] = {x - r, y + r, x + r, y - r};  // CRect: left, bottom, right, top
    Call<0x727B60>(outer, &dark);  // CSprite2d::DrawRect
    Call<0x727B60>(inner, &light);
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
