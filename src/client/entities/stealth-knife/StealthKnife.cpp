#include "entities/stealth-knife/StealthKnife.h"

#include <cmath>
#include <map>
#include <set>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"

namespace coop::knife {

namespace {
using namespace game;
using game::script::Cmd;

constexpr int WEAPON_KNIFE = 4;
constexpr int TASK_COMPLEX_KILL_PED_ON_FOOT = 1000;
std::set<CPed*> g_marked;
std::map<CPed*, uint64_t> g_knifeReady;  // a knifeman's next try
uint64_t g_victimSafeUntil = 0;          // this game's player: not twice in a row
std::map<CPed*, uint64_t> g_copySafeUntil;

bool EnsureBlock(const char* block) {
    int idx = Call<0x4D3990, int>(block);  // CAnimManager::GetAnimationBlockIndex
    if (idx < 0) return false;
    uintptr_t b = 0xB5D4A0 + (uintptr_t)idx * 0x20;  // CAnimManager::ms_aAnimBlocks
    if (Ref<uint8_t>(b + 0x10)) return true;
    RequestModel(25575 + idx, 2);  // the IFP's streaming id
    LoadAllRequestedModels(false);
    return Ref<uint8_t>(b + 0x10) != 0;
}

CPed* FightTarget(CPed* p) {
    void* intel = p->Intelligence();
    void* task = intel ? CallMethod<0x681740, void*>((uint8_t*)intel + 4, TASK_COMPLEX_KILL_PED_ON_FOOT) : nullptr;
    auto* target = task ? Field<CPed*>(task, 0x10) : nullptr;  // CTaskComplexKillPedOnFoot::m_target
    return target && IsPedValid(target) ? target : nullptr;
}

bool IsPlayerPed(CPed* p) { return p && (p == FindPlayerPed(0) || Players::Instance().FindByPed(p)); }

CVector Forward(CPed* p) {
    float h = p->GetHeading();
    return {-std::sin(h), std::cos(h), 0.0f};
}

// This game's player dies of the knife as in the game's own stealth kill: no control, the stab
// (KILL_Knife_Ped_Damage) as the knifeman's blade goes in, then the fall (TASK_DIE_NAMED_ANIM KILL_Knife_Ped_Die), and
// only when it lies on the ground the health is gone. Setting the health to nothing at once had the game take it for
// an ordinary death: CJ just dropped, with no stab and no fall of the knife.
uint64_t g_stabAt = 0, g_fallAt = 0, g_deadAt = 0;
void DieHere() {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || me->Health() <= 0.0f || me->InVehicle() || NowMs() < g_victimSafeUntil) return;
    g_victimSafeUntil = NowMs() + 6000;
    uint64_t now = NowMs();
    g_stabAt = now + 150;
    g_fallAt = now + 950;
    g_deadAt = now + 2600;
    me->Armour() = 0.0f;
    Cmd(0x01B4).I(0).I(0).Run();  // SET_PLAYER_CONTROL: off
    LOGI("Killed silently with a knife");
}

void Dying() {  // every frame while it goes on
    if (!g_stabAt && !g_fallAt && !g_deadAt) return;
    CPlayerPed* me = FindPlayerPed(0);
    uint64_t now = NowMs();
    if (!me) {
        g_stabAt = g_fallAt = g_deadAt = 0;
        return;
    }
    int h = GetPedRef(me);
    if (g_stabAt && now >= g_stabAt) {
        g_stabAt = 0;
        if (EnsureBlock("KNIFE"))
            Cmd(0x0812).I(h).S("KILL_Knife_Ped_Damage").S("KNIFE").F(8.0f).I(0).I(0).I(0).I(1).I(-1).Run();  // TASK_PLAY_ANIM_NON_INTERRUPTABLE
    }
    if (g_fallAt && now >= g_fallAt) {
        g_fallAt = 0;
        if (EnsureBlock("KNIFE")) Cmd(0x0829).I(h).S("KILL_Knife_Ped_Die").S("KNIFE").F(4.0f).I(-1).Run();  // TASK_DIE_NAMED_ANIM
    }
    if (g_deadAt && now >= g_deadAt) {
        g_deadAt = 0;
        Cmd(0x01B4).I(0).I(1).Run();  // SET_PLAYER_CONTROL: back (the death takes it anyway)
        if (me->Health() > 0.0f) me->Health() = 0.0f;
    }
}

void HandleKnife(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    if (!r.Ok() || !core::IsPlayerReady()) return;
    LOGD("Knifed by ped %04X of player %d", id, from);
    DieHere();
}

// the knifeman behind the player: in place, the kill
void Strike(CPed* knifer, CPed* victim) {
    if (!EnsureBlock("KNIFE")) return;
    CVector back = victim->GetPosition() - Forward(victim) * 0.75f;
    back.z = knifer->GetPosition().z;
    knifer->SetPositionRaw(back);
    int h = GetPedRef(knifer);
    Cmd(0x0173).I(h).F(victim->GetHeading() * 57.29578f).Run();  // SET_CHAR_HEADING: as the victim faces
    Cmd(0x094E).I(h).I(1).Run();                                    // DISABLE_CHAR_SPEECH
    Cmd(0x0605).I(h).S("KILL_Knife_Player").S("KNIFE").F(8.0f).I(0).I(0).I(0).I(0).I(-1).Run();  // TASK_PLAY_ANIM, once
    if (victim == FindPlayerPed(0)) {
        DieHere();
    } else if (RemotePlayer* rp = Players::Instance().FindByPed(victim)) {
        g_copySafeUntil[victim] = NowMs() + 6000;
        NetPed* np = Peds::Instance().Find(knifer);
        Writer w(proto::G_KNIFE);
        w.U16(np ? np->netId : 0xFFFF);
        Net().SendTo(rp->id, proto::CH_RELIABLE, w);
    }
    LOGI("A knifeman kills a player from behind");
}

void Scan() {
    CPoolHeader* pool = PedPool();
    if (!pool) return;
    uint64_t now = NowMs();
    for (auto it = g_marked.begin(); it != g_marked.end();) it = IsPedValid(*it) && (*it)->Health() > 0.0f ? std::next(it) : g_marked.erase(it);
    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* p = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
        if (p->IsPlayer() || p->Health() <= 0.0f || p->InVehicle() || p->ActiveWeapon().type != WEAPON_KNIFE) continue;
        if (Players::Instance().FindByPed(p)) continue;
        if (NetPed* np = Peds::Instance().Find(p); np && np->owner != Net().MyId() && Net().Online()) continue;  // a copy
        bool marked = g_marked.count(p) != 0;
        CPed* fight = FightTarget(p);
        if (!marked && !IsPlayerPed(fight)) continue;
        if (now < g_knifeReady[p]) continue;
        // the victim: the one it fights, or (a knifeman) any player right in front of it
        auto tryOn = [&](CPed* v) {
            if (!v || v->Health() <= 0.0f || v->InVehicle() || v->AreaCode() != p->AreaCode()) return false;
            CVector d = p->GetPosition() - v->GetPosition();
            if (std::fabs(d.z) > 0.8f) return false;
            d.z = 0;
            float dist = d.Length();
            if (dist > 1.4f || dist < 0.05f) return false;
            CVector toKnifer = d * (1.0f / dist);
            if (Forward(v).Dot(toKnifer) > -0.4f) return false;   // not behind it: the player would see
            if (Forward(p).Dot(toKnifer * -1.0f) < 0.5f) return false;  // the knifeman faces the player's back
            if (v == FindPlayerPed(0) ? now < g_victimSafeUntil : now < g_copySafeUntil[v]) return false;
            Strike(p, v);
            g_knifeReady[p] = now + 8000;
            return true;
        };
        if (IsPlayerPed(fight) && tryOn(fight)) continue;
        if (marked) {
            if (tryOn(FindPlayerPed(0))) continue;
            for (auto& [id, rp] : Players::Instance().All())
                if (rp.ped && tryOn(rp.ped)) break;
        }
    }
    for (auto it = g_knifeReady.begin(); it != g_knifeReady.end();) it = IsPedValid(it->first) ? std::next(it) : g_knifeReady.erase(it);
}

class StealthKnife : public Module {
public:
    const char* Name() const override { return "StealthKnife"; }
    void OnInstallHooks() override { Net().SetHandler(proto::G_KNIFE, &HandleKnife); }
    void OnGameShutdown() override {
        g_marked.clear();
        g_knifeReady.clear();
        g_copySafeUntil.clear();
        g_stabAt = g_fallAt = g_deadAt = 0;
    }
    void OnPostProcess() override {
        if (!core::IsPlayerReady()) return;
        Dying();
        static uint64_t scanMs = 0;
        if (NowMs() - scanMs < 100) return;
        scanMs = NowMs();
        Scan();
    }
};

StealthKnife g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

void MarkKnifeman(CPed* ped) {
    if (ped) g_marked.insert(ped);
}

}  // namespace coop::knife
