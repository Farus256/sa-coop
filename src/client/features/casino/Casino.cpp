// The casinos of Las Venturas take any bet. Their games (blackjack, roulette, the wheel of fortune, the slots, the
// Inside Track) let a player bet as much as his gambling skill allows: $1,000 for a "Gambler", up to $1,000,000 for a
// "Whale" at the skill's top (1000), which takes $100,000 spent there to reach. Their scripts ask the skill of
// CStats::GetStatValue(STAT_GAMBLING): inside a building it answers the top, so every table takes up to $1,000,000 a
// bet (the most the games have) from the first visit. The skill itself, kept and saved, grows as before.
#include <algorithm>

#include "common/lib/log/Log.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

constexpr int STAT_GAMBLING = 81;
constexpr float kWhale = 1000.0f;  // the skill's top: the biggest bets

using GetStatFn = float(__cdecl*)(int);
GetStatFn g_origGetStat = nullptr;
float __cdecl HookGetStatValue(int stat) {
    float v = g_origGetStat(stat);
    if (stat != STAT_GAMBLING) return v;
    CPlayerPed* me = FindPlayerPed(0);
    return me && me->AreaCode() != 0 ? std::max(v, kWhale) : v;  // the casinos and the betting shops are inside
}

class Casino : public Module {
public:
    const char* Name() const override { return "Casino"; }
    void OnInstallHooks() override { mem::HookFn(0x558E40, &HookGetStatValue, &g_origGetStat, "CStats::GetStatValue"); }
};

Casino g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

}  // namespace coop
