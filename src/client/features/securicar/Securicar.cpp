// The security van (Securicar). Blown up, it scatters the cash it carried: $1000-$10000 in a few wads around the wreck
// (CPickups::CreateSomeMoney, as a dead ped's money falls). The game whose van it is drops them, the others get them as
// it announces engine-made pickups (Pickups). And it is rarer in the traffic: picked as the next random car or as the
// next model to stream in, it is passed over seven times in ten.
#include <random>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

constexpr int MODEL_SECURICA = 428;
constexpr uint8_t STATUS_WRECKED = 5;
std::mt19937 g_rng{(uint32_t)NowMs()};

bool PassOver() { return g_rng() % 10 < 7; }

// CAutomobile::BlowUpCar(damager, hide the explosion)
using BlowUpFn = void(__fastcall*)(CVehicle*, void*, CEntity*, bool);
BlowUpFn g_origBlowUp = nullptr;
void __fastcall HookBlowUpCar(CVehicle* v, void* edx, CEntity* damager, bool hide) {
    bool cash = v && v->ModelIndex() == MODEL_SECURICA && (Field<uint8_t>(v, 0x36) >> 3) != STATUS_WRECKED;
    g_origBlowUp(v, edx, damager, hide);
    if (!cash) return;
    if (Net().Online()) {  // a copy of another game's van: that game drops the cash
        NetVehicle* nv = Vehicles::Instance().Find(v);
        if (nv && nv->owner != Net().MyId()) return;
    }
    int amount = 1000 + (int)(g_rng() % 9001);
    CVector at = v->GetPosition();
    Call<0x458970>(at, amount);  // CPickups::CreateSomeMoney
    LOGI("Security van blown up: $%d scattered", amount);
}

using ChooseFn = int(__cdecl*)(int*);
ChooseFn g_origChooseModel = nullptr;
int __cdecl HookChooseModel(int* cls) {
    int m = g_origChooseModel(cls);
    for (int i = 0; i < 2 && m == MODEL_SECURICA && PassOver(); ++i) m = g_origChooseModel(cls);
    return m == MODEL_SECURICA && PassOver() ? -1 : m;
}
using ChooseLoadFn = int(__cdecl*)(int);
ChooseLoadFn g_origChooseLoad = nullptr;
int __cdecl HookChooseModelToLoad(int group) {
    int m = g_origChooseLoad(group);
    for (int i = 0; i < 3 && m == MODEL_SECURICA && PassOver(); ++i) m = g_origChooseLoad(group);
    return m;
}

class Securicar : public Module {
public:
    const char* Name() const override { return "Securicar"; }
    void OnInstallHooks() override {
        mem::HookFn(0x6B3780, &HookBlowUpCar, &g_origBlowUp, "CAutomobile::BlowUpCar");
        mem::HookFn(0x424CE0, &HookChooseModel, &g_origChooseModel, "CCarCtrl::ChooseModel");
        mem::HookFn(0x421900, &HookChooseModelToLoad, &g_origChooseLoad, "CCarCtrl::ChooseCarModelToLoad");
    }
};

Securicar g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

}  // namespace coop
