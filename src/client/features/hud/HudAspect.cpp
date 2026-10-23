// The HUD in its own proportions on a wide screen (shared/game/HudScale.h): the parts of the frame that are the HUD's
// are drawn between hud::Begin and hud::End. CHud::Draw itself is Players' hook (it draws the other players' icons over
// their heads after it, in the world's places); here are the rest: what CHud draws after the fade, the game's messages
// (CMessages::Display: briefs, big messages, subtitles), the rampage and garage texts, the radio station's name. The
// crosshair and the scopes (CHud::DrawCrossHairs) are where the camera aims: not narrowed.
#include "common/lib/log/Log.h"
#include "shared/game/Game.h"
#include "shared/game/HudScale.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using VoidFn = void(__cdecl*)();
using BoolArgFn = void(__cdecl*)(bool);
using ThisFn = void(__fastcall*)(void*, void*);

VoidFn g_origAfterFade = nullptr, g_origDarkel = nullptr, g_origGarages = nullptr, g_origCrossHairs = nullptr;
BoolArgFn g_origMessages = nullptr;
ThisFn g_origRadioName = nullptr;

void __cdecl HookAfterFade() {
    hud::Begin();
    g_origAfterFade();
    hud::End();
}
void __cdecl HookMessages(bool flag) {
    hud::Begin();
    g_origMessages(flag);
    hud::End();
}
void __cdecl HookDarkel() {
    hud::Begin();
    g_origDarkel();
    hud::End();
}
void __cdecl HookGarages() {
    hud::Begin();
    g_origGarages();
    hud::End();
}
void __fastcall HookRadioName(void* self, void* edx) {
    hud::Begin();
    g_origRadioName(self, edx);
    hud::End();
}
void __cdecl HookCrossHairs() {
    hud::Suspend();
    g_origCrossHairs();
    hud::Resume();
}

class HudAspect : public Module {
public:
    const char* Name() const override { return "HudAspect"; }
    void OnInstallHooks() override {
        hud::Install();
        mem::HookFn(0x58D490, &HookAfterFade, &g_origAfterFade, "CHud::DrawAfterFade");
        mem::HookFn(0x69EFC0, &HookMessages, &g_origMessages, "CMessages::Display");
        mem::HookFn(0x43CEC0, &HookDarkel, &g_origDarkel, "CDarkel::DrawMessages");
        mem::HookFn(0x447790, &HookGarages, &g_origGarages, "CGarages::PrintMessages");
        mem::HookFn(0x4E9E50, &HookRadioName, &g_origRadioName, "CAERadioTrackManager::DisplayRadioStationName");
        mem::HookFn(0x58E020, &HookCrossHairs, &g_origCrossHairs, "CHud::DrawCrossHairs");
    }
};

HudAspect g_hudAspect;
struct Reg {
    Reg() { core::RegisterModule(&g_hudAspect); }
} g_reg;
}  // namespace

}  // namespace coop
