// The more players, the harder the fight. With N players in the game (N-1 more than the story was made for):
//  * every armed person this game makes or copies shoots more accurately: CPed::m_nWeaponAccuracy times
//    1 + 0.25 (N-1), at most 95;
//  * the police send more: each wanted level's cops and cop cars in pursuit (CWanted::UpdateWantedLevel's
//    m_MaxCopsInPursuit, m_MaxCopCarsInPursuit) grow by half for each player more (at most 15 cops, 5 cars);
//  * the host's mission's enemies come with reinforcements: when one of its people starts a fight with a player
//    (CTaskComplexKillPedOnFoot on a player), a game with more players makes extra ones of the same model and gun near
//    it, behind it as the player sees it, and sets them on the same player (one for every two players more, at most 8
//    at a time). They are ordinary people of the host's game, the others see them as its copies.
// The police come armed: a cop after a wanted player with nothing in hand gets its nightstick (one star, the player
// unarmed) or its pistol (shotgun if it has one). The game chooses a pursuing cop's weapon by the wanted level of
// its own player (CTaskComplexPolicePursuit::SetWeapon, FindPlayerWanted): a cop of this game after another player,
// whose stars this game's player has not, kept its fists; and the pistol comes "delayed" (CCopPed::CCopPed), only once
// its model is in memory. Every 0.5 s, online or not.
// The SWAT and the FBI (models 285, 286) are sharpshooters who see far: accuracy 95, shooting range 120, sight 80 m,
// full armour, and a headshot does not kill them at once (SET_CHAR_SUFFERS_CRITICAL_HITS off).
// And the SWAT carry the SMG the FBI do (the MP5, held in both hands as police hold their guns) instead of the Micro
// Uzi they had, shot sideways the way gangsters shoot it (always, online or not).
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

constexpr int TASK_COMPLEX_KILL_PED_ON_FOOT = 1000;
constexpr uint8_t PED_MISSION = 2;
constexpr int kWantedMaxCops = 0x19, kWantedMaxCopCars = 0x1A;  // CWanted
constexpr int kMaxExtras = 8;
constexpr int kLastWeapon = 46;

// CCopPed::CCopPed(type or model)
constexpr uint32_t COP_TYPE_SWAT1 = 2, COP_TYPE_SWAT2 = 3;
constexpr int WEAPON_MP5 = 29, MODEL_MP5 = 353;
using CopCtorFn = void*(__fastcall*)(void*, void*, uint32_t);
CopCtorFn g_origCopCtor = nullptr;
void* __fastcall HookCopCtor(void* cop, void* edx, uint32_t type) {
    void* r = g_origCopCtor(cop, edx, type);
    if (type == COP_TYPE_SWAT1 || type == COP_TYPE_SWAT2) {
        RequestModel(MODEL_MP5);
        CallMethod<0x5E89B0>(cop, WEAPON_MP5, 1000u);  // CPed::GiveDelayedWeapon: the SMG slot, the Uzi's
        CallMethod<0x5E6280>(cop, WEAPON_MP5);         // CPed::SetCurrentWeapon(type)
    }
    return r;
}

// ---- the police's arms
constexpr int PED_TYPE_COP = 6, WEAPON_NIGHTSTICK = 3, WEAPON_PISTOL = 22, WEAPON_SHOTGUN = 25;
uint64_t g_copScanMs = 0;

uint8_t LocalWanted() {
    void* w = Call<0x56E230, void*>(-1);  // FindPlayerWanted
    return w ? (uint8_t)std::clamp(Field<int32_t>(w, 0x2C), 0, 6) : 0;
}

bool Has(CPed* p, int weapon) { return CallMethod<0x5DF300, bool>(p, weapon); }  // CPed::DoWeHaveWeaponAvailable

// the nearest wanted player within 45 m of the cop, its stars and whether it holds a gun
CPed* WantedNear(CPed* cop, uint8_t& stars) {
    CPed* best = nullptr;
    float bestD = 45.0f;
    auto consider = [&](CPed* ped, uint8_t wanted) {
        if (!ped || !wanted || ped->Health() <= 0.0f) return;
        float d = (ped->GetPosition() - cop->GetPosition()).Length();
        if (d < bestD) bestD = d, best = ped, stars = wanted;
    };
    consider(FindPlayerPed(0), LocalWanted());
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.ped && rp.hasState) consider(rp.ped, rp.state.wanted);
    return best;
}

std::set<int> g_elite;  // handles made elite
void Elite() {
    CPoolHeader* pool = PedPool();
    if (!pool) return;
    for (auto it = g_elite.begin(); it != g_elite.end();) it = GetPed(*it) ? std::next(it) : g_elite.erase(it);
    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* p = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
        int model = p->ModelIndex();
        if ((model != 285 && model != 286) || p->IsPlayer() || p->Health() <= 0.0f) continue;
        if (NetPed* np = Peds::Instance().Find(p); np && np->owner != Net().MyId() && Net().Online()) continue;  // a copy
        int h = GetPedRef(p);
        if (!g_elite.insert(h).second) continue;
        Cmd(0x0446).I(h).I(0).Run();     // SET_CHAR_SUFFERS_CRITICAL_HITS: no one-shot headshots
        Cmd(0x060F).I(h).F(80.0f).Run();  // SET_SENSE_RANGE
        p->WeaponAccuracy() = 95;
        Field<uint8_t>(p, 0x719) = 120;   // CPed::m_nWeaponShootingRate: how far it opens fire
        if (p->Armour() < 50.0f) p->Armour() = 50.0f;  // half a vest (a full one with no headshots was too much)
    }
}

void ArmCops() {
    CPoolHeader* pool = PedPool();
    if (!pool) return;
    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* p = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
        if (p->PedType() != PED_TYPE_COP || p->IsPlayer() || p->Health() <= 0.0f || p->InVehicle() || p->ActiveWeapon().type != 0)
            continue;
        if (NetPed* np = Peds::Instance().Find(p); np && np->owner != Net().MyId() && Net().Online()) continue;  // a copy: its game arms it
        uint8_t stars = 0;
        CPed* target = WantedNear(p, stars);
        if (!target) continue;
        int t = target->ActiveWeapon().type;
        bool armed = t >= 16 && t <= 38;  // a thrown weapon or a gun
        int want = stars == 1 && !armed ? WEAPON_NIGHTSTICK : Has(p, WEAPON_SHOTGUN) ? WEAPON_SHOTGUN : WEAPON_PISTOL;
        if (!Has(p, want)) {
            int model = -1;
            Cmd(0x0781).I(want).Out(&model).Run();  // GET_WEAPONTYPE_MODEL
            if (model > 0 && !IsModelLoaded(model)) {
                RequestModel(model);
                continue;  // next time
            }
            Cmd(0x01B2).I(GetPedRef(p)).I(want).I(want == WEAPON_NIGHTSTICK ? 1 : 200).Run();  // GIVE_WEAPON_TO_CHAR
        }
        Cmd(0x01B9).I(GetPedRef(p)).I(want).Run();  // SET_CURRENT_CHAR_WEAPON
        LOGD("Cop %p armed with %d (%d stars)", (void*)p, want, stars);
    }
}

int PlayersInGame() {
    int n = 1;
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.hasState && NowMs() - rp.stateRecvMs < 5000) ++n;
    return n;
}

// ---- accuracy
std::map<CPed*, int> g_sharpened;  // the people made sharper, by their handle (a reused slot is another person)
uint64_t g_scanMs = 0;

bool Armed(CPed* p) {
    int t = p->ActiveWeapon().type;
    return t >= 22 && t <= 38;  // pistols .. the minigun
}

void Sharpen(int players) {
    CPoolHeader* pool = PedPool();
    if (!pool) return;
    float k = 1.0f + 0.25f * (players - 1);
    for (auto it = g_sharpened.begin(); it != g_sharpened.end();)
        it = IsPedValid(it->first) && GetPedRef(it->first) == it->second ? std::next(it) : g_sharpened.erase(it);
    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* p = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
        if (p->IsPlayer() || p->Health() <= 0.0f || !Armed(p) || g_sharpened.count(p)) continue;
        uint8_t& acc = p->WeaponAccuracy();
        uint8_t was = acc;
        acc = (uint8_t)std::min(95.0f, std::max(1.0f, (float)acc) * k);
        g_sharpened[p] = GetPedRef(p);
        LOGD("Ped %p accuracy %d -> %d (%d players)", (void*)p, was, acc, players);
    }
}

// ---- the police
using UpdateWantedFn = void(__fastcall*)(void*, void*);
UpdateWantedFn g_origUpdateWanted = nullptr;
void __fastcall HookUpdateWantedLevel(void* wanted, void* edx) {
    g_origUpdateWanted(wanted, edx);
    if (!Net().Online()) return;
    int more = PlayersInGame() - 1;
    if (more <= 0) return;
    uint8_t& cops = Field<uint8_t>(wanted, kWantedMaxCops);
    uint8_t& cars = Field<uint8_t>(wanted, kWantedMaxCopCars);
    if (cops) cops = (uint8_t)std::min(15, cops + (cops * more + 1) / 2);
    if (cars) cars = (uint8_t)std::min(5, cars + (more + 1) / 2);
}

// ---- the host's mission's enemies
std::set<int> g_fought;   // mission people (handles) whose fight was seen (reinforced or not)
std::set<int> g_extras;   // the reinforcements alive
uint64_t g_fightScanMs = 0;

CPed* KillTarget(CPed* p) {
    void* intel = p->Intelligence();
    void* task = intel ? CallMethod<0x681740, void*>((uint8_t*)intel + 4, TASK_COMPLEX_KILL_PED_ON_FOOT) : nullptr;
    auto* target = task ? Field<CPed*>(task, 0x10) : nullptr;  // CTaskComplexKillPedOnFoot::m_target
    if (!target || !IsPedValid(target)) return nullptr;
    return target == FindPlayerPed(0) || Players::Instance().FindByPed(target) ? target : nullptr;
}

void Reinforce(CPed* enemy, CPed* target, int count) {
    int model = enemy->ModelIndex(), weapon = enemy->ActiveWeapon().type, type = enemy->PedType();
    if (!IsPedModel(model) || model >= 290 || weapon <= 0 || weapon > kLastWeapon) return;  // a special character: no copies
    if (!LoadModelNow(model)) return;
    int wmodel = -1;
    Cmd(0x0781).I(weapon).Out(&wmodel).Run();  // GET_WEAPONTYPE_MODEL
    if (wmodel > 0) LoadModelNow(wmodel);
    CVector at = enemy->GetPosition(), away = at - target->GetPosition();
    float len = std::max(0.1f, away.Length2D());
    CVector back{away.x / len, away.y / len, 0}, side{-back.y, back.x, 0};
    int targetHandle = GetPedRef(target);
    for (int i = 0; i < count && (int)g_extras.size() < kMaxExtras; ++i) {
        if (!HasRoom(PedPool(), kKeepPeds + 4, "ped")) return;
        float s = (i % 2 ? -1.0f : 1.0f) * (1.5f + i), b = 2.5f + i * 0.8f;
        CVector p = at + back * b + side * s;
        bool found = false;
        float ground = Call<0x5696C0, float>(p.x, p.y, p.z + 2.0f, &found, (CEntity**)nullptr);  // FindGroundZFor3DCoord
        if (!found || std::fabs(ground - at.z) > 3.0f) continue;
        int h = -1;
        Cmd(0x009A).I(type).I(model).F(p.x).F(p.y).F(ground).Out(&h).Run();  // CREATE_CHAR
        if (h < 0) continue;
        Cmd(0x01B2).I(h).I(weapon).I(9999).Run();          // GIVE_WEAPON_TO_CHAR
        Cmd(0x01B9).I(h).I(weapon).Run();                  // SET_CURRENT_CHAR_WEAPON
        if (CPed* e = GetPed(h)) e->WeaponAccuracy() = enemy->WeaponAccuracy();
        Cmd(0x05E2).I(h).I(targetHandle).Run();            // TASK_KILL_CHAR_ON_FOOT
        Cmd(0x01C2).I(h).Run();                            // MARK_CHAR_AS_NO_LONGER_NEEDED: the game's own once gone
        g_extras.insert(h);
        g_fought.insert(h);
    }
    MarkModelNotNeeded(model);
}

void WatchFights(int players) {
    int per = (players - 1 + 1) / 2;  // one for every two players more
    for (auto it = g_extras.begin(); it != g_extras.end();) {
        CPed* e = GetPed(*it);
        it = e && e->Health() > 0.0f ? std::next(it) : g_extras.erase(it);
    }
    if (per <= 0 || !Net().IsHost() || !HostReplicating()) return;
    CPoolHeader* pool = PedPool();
    if (!pool) return;
    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* p = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
        if (p->IsPlayer() || p->CreatedBy() != PED_MISSION || p->Health() <= 0.0f || p->InVehicle() || !Armed(p)) continue;
        int h = GetPedRef(p);
        if (g_fought.count(h)) continue;
        if (NetPed* np = Peds::Instance().Find(p); np && np->owner != Net().MyId()) continue;  // another game's
        CPed* target = KillTarget(p);
        if (!target) continue;
        g_fought.insert(h);
        Reinforce(p, target, per);
        LOGI("Mission enemy %d fights a player: %d reinforcement(s) for %d players", h, per, players);
    }
}

class Difficulty : public Module {
public:
    const char* Name() const override { return "Difficulty"; }
    void OnInstallHooks() override {
        mem::HookFn(0x561C90, &HookUpdateWantedLevel, &g_origUpdateWanted, "CWanted::UpdateWantedLevel");
        mem::HookFn(0x5DDC60, &HookCopCtor, &g_origCopCtor, "CCopPed::CCopPed");
    }
    void OnGameStarted() override { Reset(); }
    void OnSessionEnd() override { Reset(); }
    void OnPostProcess() override {
        if (!core::IsPlayerReady()) return;
        if (NowMs() - g_copScanMs >= 500) {
            g_copScanMs = NowMs();
            ArmCops();
            Elite();
        }
        if (!Net().Online()) return;
        int players = PlayersInGame();
        if (players <= 1) return;
        uint64_t now = NowMs();
        if (now - g_scanMs >= 500) {
            g_scanMs = now;
            Sharpen(players);
        }
        if (now - g_fightScanMs >= 300) {
            g_fightScanMs = now;
            WatchFights(players);
        }
        if (!HostReplicating()) g_fought.clear();  // a new mission: its people are new
    }

private:
    static void Reset() {
        g_sharpened.clear();
        g_fought.clear();
        g_extras.clear();
    }
};

Difficulty g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

}  // namespace coop
