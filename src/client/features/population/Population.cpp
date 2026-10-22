// Population control for co-op:
//  * where players are together, one game generates the random peds, traffic and parked cars for all of them (they see
//    its population through Peds/Vehicles sync): the host's, else the lowest-numbered one's that generates its own;
//    a wanted player's game makes its own (police chases);
//  * entities owned by this game are not removed while the other player is close to them
//    (CPopulation::ManagePed / CCarCtrl::PossiblyRemoveVehicle measure distance to the nearest player);
//  * farther from this game's player, an ordinary ped or car the other player is nearer to goes to the other game
//    instead of being dropped here (it vanished in front of the other player).
#include <algorithm>
#include <cmath>

#include "features/population/Population.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "entities/vehicle/Vehicles.h"
#include "entities/world-share/WorldShare.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

// a player closer than this to one whose game generates: that population (spawned 40-55 m around it) covers both
constexpr float kShareRadius = 60.0f;
constexpr float kShareRadiusOff = 90.0f;  // hysteresis

bool g_suppress = false;

int LocalWantedLevel() {
    void* wanted = Call<0x56E230, void*>(-1);  // FindPlayerWanted
    return wanted ? Field<int32_t>(wanted, 0x2C) : 0;
}

// A game leaves the population to a player before it (MakesBefore) whose game makes its own (its state says so) close
// by; one that leaves it too does not count, so a line of players apart gets people all along.
void UpdateSuppression() {
    bool want = false;
    uint8_t by = 255;
    if (Net().Online() && !Net().IsHost() && core::IsPlayerReady() && LocalWantedLevel() == 0) {
        CPlayerPed* me = FindPlayerPed(0);
        float limit = g_suppress ? kShareRadiusOff : kShareRadius;
        for (auto& [id, rp] : Players::Instance().All()) {
            if (!rp.hasState || !(rp.state.flags & OnFootState::F_POPULATION) || !MakesBefore(id) ||
                NowMs() - rp.stateRecvMs > 3000 || rp.state.area != me->AreaCode())
                continue;
            if ((rp.state.pos - me->GetPosition()).Length2D() < limit) {
                want = true;
                by = id;
                break;
            }
        }
    }
    if (want != g_suppress)
        LOGI("Population generation %s", want ? Format("handed to player %d", by).c_str() : "local");
    Share().population = !want;
    if (g_suppress && !want) {
        // the engine adds one ped a frame at the edge of view: the empty streets around would fill slowly. The
        // countdowns of a game's start (and of a respawn) fill them at once
        Ref<uint8_t>(0xC0EC23) = 2;  // CPopulation::m_CountDownToPedsAtStart
        Ref<uint8_t>(0x9690C0) = 2;  // CCarCtrl::CountDownToCarsAtStart
    }
    g_suppress = want;
}

using UpdatePopFn = void(__cdecl*)(bool);
UpdatePopFn g_origPopUpdate = nullptr;
void __cdecl HookPopulationUpdate(bool generate) { g_origPopUpdate(generate && !g_suppress); }

using VoidFn = void(__cdecl*)();
VoidFn g_origGenerateRandomCars = nullptr;
void __cdecl HookGenerateRandomCars() {
    if (g_suppress) return;
    g_origGenerateRandomCars();
}

VoidFn g_origCarGenerators = nullptr;
void __cdecl HookCarGeneratorsProcess() {
    if (g_suppress) return;
    g_origCarGenerators();
}

// the other player is in the game (states keep coming): what it is given goes nowhere while it loads a world
bool CanTake(uint8_t who) {
    const RemotePlayer* rp = Players::Instance().Get(who);
    return rp && rp->ped && NowMs() - rp->stateRecvMs < 1500;
}

// nearest remote player position (same interior) within maxDist, or nullptr
const CVector* NearestRemote(const CVector& p, float maxDist, int8_t area, uint8_t* who = nullptr) {
    const CVector* best = nullptr;
    float bestD = maxDist;
    for (auto& [id, rp] : Players::Instance().All()) {
        if (!rp.hasState || rp.state.area != area) continue;
        float d = (rp.state.pos - p).Length2D();
        if (d < bestD) {
            bestD = d;
            best = &rp.state.pos;
            if (who) *who = id;
        }
    }
    return best;
}

using ManagePedFn = void(__cdecl*)(CPed*, const CVector*);
ManagePedFn g_origManagePed = nullptr;
void __cdecl HookManagePed(CPed* ped, const CVector* centre) {
    if (Net().Online() && ped && centre) {
        CVector p = ped->GetPosition();
        float local = (p - *centre).Length2D();
        uint8_t who = 0;
        if (const CVector* r = NearestRemote(p, local, AreaOf(ped), &who)) {
            // far from this game's player and clearly nearer the other one: the other game keeps it (the margin keeps a
            // ped from going back and forth between two players standing together)
            if (local > 60.0f && (p - *r).Length2D() < local - 30.0f && CanTake(who) && Peds::Instance().QueueGiveAway(ped, who))
                return;
            // the other player still needs this ped, yet the engine culls by this game's camera too (off screen and
            // over 25 m: gone in 4 s). While this game has ground under it (collision loads ~120 m around its
            // player), a live one near the other player stays; farther it would fall through the map here, so it
            // goes as usual and the other player's own population takes over
            if (local < 100.0f && (p - *r).Length2D() < 50.0f && ped->Health() > 0.0f) return;
            g_origManagePed(ped, r);
            return;
        }
    }
    g_origManagePed(ped, centre);
}

using PossiblyRemoveFn = void(__cdecl*)(CVehicle*);
PossiblyRemoveFn g_origPossiblyRemove = nullptr;
void __cdecl HookPossiblyRemoveVehicle(CVehicle* v) {
    // kept for the other player while this game has ground under it (see HookManagePed), then given to it; a wreck
    // goes as usual (kept, they filled the vehicle pool around two players)
    uint8_t who = 0;
    if (Net().Online() && v && v->Status() != 5 /* STATUS_WRECKED */)
        if (const CVector* r = NearestRemote(v->GetPosition(), 140.0f, v->AreaCode(), &who)) {
            float local = (v->GetPosition() - FindPlayerPed(0)->GetPosition()).Length2D();
            if (local < 100.0f) return;
            if ((v->GetPosition() - *r).Length2D() < local - 40.0f && CanTake(who) && Vehicles::Instance().QueueGiveAway(v, who))
                return;
        }
    g_origPossiblyRemove(v);
}

// The people of an entrance's interior (CEntryExit::GenerateAmbientPeds: a police station's officers, a bar's
// customers) come in the game of the player who is there first, and the other player sees that game's (Peds): each game
// had its own, and none of the other's.
// `at`: where in the interior (one area holds several interiors far apart)
bool OtherPlayerInside(const CVector& at) {
    int area = Ref<int32_t>(0xB72914);  // CGame::currArea: the interior this game's player is in (or just came into)
    if (!Net().Online() || !core::IsPlayerReady() || area == 0) return false;
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.hasState && rp.state.area == area && (rp.state.pos - at).Length() < 80.0f) return true;
    return false;
}

using PopulateInteriorFn = void(__cdecl*)(int, CVector);
PopulateInteriorFn g_origPopulateInterior = nullptr;
void __cdecl HookPopulateInterior(int count, CVector at) {  // CEntryExit::GenerateAmbientPeds, on coming in
    if (OtherPlayerInside(at)) {  // our player is still at the door outside, `at` is in the interior
        LOGI("The interior's people are the other game's (%d not made here)", count);
        return;
    }
    g_origPopulateInterior(count, at);
}

// The furnished interiors (InteriorGroup_c: the shops' cashiers and customers, the houses' residents, the offices'
// workers) are each game's own. They live off their interior's own logic (the cashier who puts his hands up and pays out
// at gunpoint, the residents who wake up at a burglar), and the copy of another game's cashier reacted to nothing: the
// player who came in second could not rob the shop, nor deal with anyone in it. Every game makes its own and none is
// announced (Peds::KeepLocal).
constexpr int kGroupNumPeds = 0x36, kGroupPeds = 0x38, kGroupMaxPeds = 16;  // InteriorGroup_c::m_numPeds, m_peds[16]
using SetupPedsFn = void(__fastcall*)(void*, void*);
SetupPedsFn g_origSetupInteriorPeds = nullptr;
void __fastcall HookSetupInteriorPeds(void* group, void* edx) {  // InteriorGroup_c::Update asks again every frame
    g_origSetupInteriorPeds(group, edx);
    if (!group) return;
    int n = std::clamp<int>(Field<int8_t>(group, kGroupNumPeds), 0, kGroupMaxPeds);
    for (int i = 0; i < n; ++i)
        if (CPed* p = Field<CPed*>(group, kGroupPeds + i * 4)) Peds::Instance().KeepLocal(p);
}

class Population : public Module {
public:
    const char* Name() const override { return "Population"; }
    void OnInstallHooks() override {
        mem::RedirectCallFn(0x59690F, &HookSetupInteriorPeds, &g_origSetupInteriorPeds);  // InteriorGroup_c::Update
        mem::RedirectCallFn(0x43E983, &HookPopulateInterior, &g_origPopulateInterior);    // CEntryExit::GenerateAmbientPeds
        mem::HookFn(0x616650, &HookPopulationUpdate, &g_origPopUpdate, "CPopulation::Update");
        mem::HookFn(0x4341C0, &HookGenerateRandomCars, &g_origGenerateRandomCars, "CCarCtrl::GenerateRandomCars");
        mem::HookFn(0x6F3F40, &HookCarGeneratorsProcess, &g_origCarGenerators, "CTheCarGenerators::Process");
        mem::HookFn(0x611FC0, &HookManagePed, &g_origManagePed, "CPopulation::ManagePed");
        mem::HookFn(0x424F80, &HookPossiblyRemoveVehicle, &g_origPossiblyRemove, "CCarCtrl::PossiblyRemoveVehicle");
    }
    void OnPreProcess() override { UpdateSuppression(); }
    void OnSessionEnd() override {
        g_suppress = false;
        Share().population = true;
    }
};

Population g_pop;
struct Reg {
    Reg() { core::RegisterModule(&g_pop); }
} g_reg;
}  // namespace

bool PopulationSuppressed() { return g_suppress; }

}  // namespace coop
