#include "features/revive/Revive.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <deque>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

constexpr uint64_t kDownMs = 30000;              // how long a wounded player waits for help
constexpr uint64_t kCprMs = 5000;                // how long the CPR takes
constexpr uint64_t kWindowMs = 6 * 60 * 1000;    // revives are counted over this time
constexpr int kMaxRevives = 2;                   // ... and this many are allowed in it
constexpr uint64_t kGiveUpHoldMs = 1500;         // holding the key this long, the wounded one gives up
constexpr float kHelpRadius = 200.0f;            // a wounded player waits only with a partner this near
constexpr float kReach = 1.8f, kKeepReach = 3.0f;  // how near the rescuer must be to start, and to go on
constexpr uint16_t kOwnControls = 0x8000;        // our bit of CPad::DisablePlayerControls (the game uses the low ones)
constexpr uint32_t kProofBits = (1u << 18) | (1u << 19) | (1u << 20) | (1u << 21) | (1u << 23);  // bullet..explosion
constexpr uint32_t kSubmerged = 1u << 8;         // CPhysical::physicalFlags.bSubmergedInWater
constexpr int WEAPON_DROWNING = 53;
constexpr uintptr_t kCutsceneRunning = 0xB5F851;

enum : uint8_t { OP_START = 1, OP_STOP = 2, OP_DONE = 3 };

// ---- this player wounded
bool g_wantDown = false;   // its health ran out in the damage processing: it falls right after it
bool g_down = false;
uint64_t g_downEnd = 0;
uint32_t g_savedProofs = 0;
int g_rescuer = -1;        // who gives it CPR now
uint64_t g_animCheckMs = 0, g_holdMs = 0;
std::deque<uint64_t> g_revives;  // when it was revived

// ---- this player reviving another
int g_target = -1;
uint64_t g_cprStart = 0;
int g_candidate = -1;      // the wounded one near enough to revive now

CPad* Pad() { return GetPad(0); }
std::string Key() {  // the revive key as the player knows it: a letter, a digit, its name on the keyboard
    int vk = Cfg().reviveKey;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, (char)vk);
    char name[32] = {};
    if (GetKeyNameTextA((LONG)(MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC) << 16), name, sizeof name) > 0) return name;
    return Format("#%d", vk);
}
std::string Name(int id) { return Net().PeerName((uint8_t)id); }

int RevivesLeft() {
    uint64_t now = NowMs();
    while (!g_revives.empty() && now - g_revives.front() > kWindowMs) g_revives.pop_front();
    return kMaxRevives - (int)g_revives.size();
}

// a partner who can come: alive, not wounded, in the same place, near
bool HelpNear(CPlayerPed* me) {
    for (auto& [id, rp] : Players::Instance().All()) {
        const OnFootState& st = rp.state;
        if (!rp.hasState || NowMs() - rp.stateRecvMs > 3000 || (st.flags & (OnFootState::F_DEAD | OnFootState::F_DOWNED)))
            continue;
        if (st.area == me->AreaCode() && (st.pos - me->GetPosition()).Length() < kHelpRadius) return true;
    }
    return false;
}

bool CanGoDown(CPlayerPed* me, int weapon) {
    if (weapon == WEAPON_DROWNING || me->InVehicle() || (me->PhysicalFlags() & kSubmerged) || Ref<uint8_t>(kCutsceneRunning))
        return false;
    if (!HelpNear(me)) return false;
    if (RevivesLeft() <= 0) {
        ui::TheChat().AddSystem(TR("Вас уже откачивали дважды за 6 минут: в этот раз не повезло.",
                                   "You were revived twice in 6 minutes already: no luck this time."));
        return false;
    }
    return true;
}

void SendOp(int to, uint8_t op) {
    Writer w(proto::G_REVIVE);
    w.U8(op);
    Net().SendTo((uint8_t)to, proto::CH_RELIABLE, w);
}

void LieDown(CPlayerPed* me) {
    Cmd(0x0605).I(GetPedRef(me)).S(kDownAnim).S("PED").F(4.0f).I(0).I(0).I(0).I(1).I(-1).Run();  // TASK_PLAY_ANIM, held
}

// CPedDamageResponseCalculator::ComputeWillKillPed(ped, response, speak): the hit that ends this player's health leaves
// it 1 (CPedDamageResponse: m_bHealthZero +8, m_bForceDeath +9; the calculator's weapon +0xC)
using WillKillFn = void(__fastcall*)(void*, void*, CPed*, uint8_t*, bool);
WillKillFn g_origWillKill = nullptr;
void __fastcall HookWillKill(void* calc, void* edx, CPed* ped, uint8_t* rsp, bool speak) {
    g_origWillKill(calc, edx, ped, rsp, speak);
    if (!rsp[8] || !Net().Online()) return;
    CPlayerPed* me = FindPlayerPed(0);
    if (ped != me) return;
    if (!g_down && !g_wantDown && !CanGoDown(me, Field<int32_t>(calc, 0x0C))) return;
    ped->Health() = 1.0f;
    rsp[8] = rsp[9] = 0;
    g_wantDown = true;
}

void GoDown(CPlayerPed* me) {
    g_wantDown = false;
    if (g_down) return;
    if (me->AttachedTo()) DetachPed(me);
    g_down = true;
    g_downEnd = NowMs() + kDownMs;
    g_rescuer = -1;
    g_holdMs = 0;
    g_savedProofs = me->PhysicalFlags() & kProofBits;
    me->PhysicalFlags() |= kProofBits;  // nobody finishes it off: it bleeds out or is revived
    Pad()->DisablePlayerControls |= kOwnControls;
    Cmd(0x0792).I(GetPedRef(me)).Run();  // CLEAR_CHAR_TASKS_IMMEDIATELY
    LieDown(me);
    g_animCheckMs = NowMs();
    Players::Instance().SetLocalDowned(true, (uint8_t)(kDownMs / 1000));
    ui::TheChat().AddSystem(Format(TR("Вы тяжело ранены! Напарник может откачать вас за %d с. Удерживайте %s, чтобы сдаться.",
                                      "You are badly wounded! A partner can revive you within %d s. Hold %s to give up."),
                                   (int)(kDownMs / 1000), Key().c_str()));
    LOGI("Wounded: lying for %llu s (revives left %d)", (unsigned long long)(kDownMs / 1000), RevivesLeft());
}

void EndDown(CPlayerPed* me) {
    g_down = false;
    g_wantDown = false;
    g_rescuer = -1;
    if (me) me->PhysicalFlags() = (me->PhysicalFlags() & ~kProofBits) | g_savedProofs;
    Pad()->DisablePlayerControls &= (uint16_t)~kOwnControls;
    Players::Instance().SetLocalDowned(false);
}

void Die(CPlayerPed* me, const char* why) {
    EndDown(me);
    Cmd(0x0223).I(GetPedRef(me)).I(0).Run();  // SET_CHAR_HEALTH 0: the death task
    me->Health() = 0.0f;
    LOGI("Wounded: died (%s)", why);
}

void Revive(CPlayerPed* me, uint8_t by) {
    EndDown(me);
    int handle = GetPedRef(me);
    Cmd(0x0792).I(handle).Run();  // CLEAR_CHAR_TASKS_IMMEDIATELY
    Cmd(0x0605).I(handle).S(kGetUpAnim).S("PED").F(4.0f).I(0).I(0).I(0).I(0).I(-1).Run();  // TASK_PLAY_ANIM, once
    float maxHealth = std::max<float>(PlayerInfos()[0].maxHealth, 100.0f);
    me->Health() = maxHealth * 0.5f;
    g_revives.push_back(NowMs());
    int left = RevivesLeft();
    ui::TheChat().AddSystem(Format(TR("%s откачал вас. Ещё возрождений в ближайшие 6 минут: %d.",
                                      "%s revived you. Revives left for the next 6 minutes: %d."),
                                   Name(by).c_str(), left));
    LOGI("Revived by player %d (revives left %d)", by, left);
}

// ---- the rescuer
bool EnsureBlock(const char* block) {
    int idx = Call<0x4D3990, int>(block);  // CAnimManager::GetAnimationBlockIndex
    if (idx < 0) return false;
    uintptr_t b = 0xB5D4A0 + (uintptr_t)idx * 0x20;  // CAnimManager::ms_aAnimBlocks
    if (Ref<uint8_t>(b + 0x10)) return true;
    RequestModel(25575 + idx, 2);  // the IFP's streaming id
    LoadAllRequestedModels(false);
    return Ref<uint8_t>(b + 0x10) != 0;
}

bool CanHelp(CPlayerPed* me) {
    return me->Health() > 0.0f && !g_down && !g_wantDown && !me->InVehicle() && !me->AttachedTo() &&
           !Ref<uint8_t>(kCutsceneRunning) && !(Pad()->DisablePlayerControls & ~kOwnControls);
}

const RemotePlayer* Wounded(int id) {
    const RemotePlayer* rp = Players::Instance().Get((uint8_t)id);
    return rp && rp->ped && rp->hasState && (rp->state.flags & OnFootState::F_DOWNED) ? rp : nullptr;
}

void StopCpr(CPlayerPed* me, bool tell) {
    if (g_target < 0) return;
    if (tell) SendOp(g_target, OP_STOP);
    g_target = -1;
    Pad()->DisablePlayerControls &= (uint16_t)~kOwnControls;
    Players::Instance().SetLocalReviving(false);
    if (me) Cmd(0x0792).I(GetPedRef(me)).Run();  // CLEAR_CHAR_TASKS_IMMEDIATELY: the CPR ends
}

void StartCpr(CPlayerPed* me, int id) {
    const RemotePlayer* rp = Wounded(id);
    if (!rp || !EnsureBlock("MEDIC")) return;
    int handle = GetPedRef(me);
    CVector d = rp->ped->GetPosition() - me->GetPosition();
    Cmd(0x0173).I(handle).F(std::atan2(-d.x, d.y) * 57.29578f).Run();  // SET_CHAR_HEADING: to the wounded one
    Cmd(0x0605).I(handle).S("CPR").S("MEDIC").F(4.0f).I(1).I(0).I(0).I(0).I(-1).Run();  // TASK_PLAY_ANIM, looped
    g_target = id;
    g_cprStart = NowMs();
    Pad()->DisablePlayerControls |= kOwnControls;
    Players::Instance().SetLocalReviving(true);
    SendOp(id, OP_START);
    LOGI("CPR for player %d", id);
}

void UpdateRescuer(CPlayerPed* me) {
    g_candidate = -1;
    if (g_target >= 0) {
        const RemotePlayer* rp = Wounded(g_target);
        bool keep = rp && me->Health() > 0.0f && !g_down && !g_wantDown && !me->InVehicle() &&
                    rp->ped->AreaCode() == me->AreaCode() &&
                    (rp->ped->GetPosition() - me->GetPosition()).Length() < kKeepReach;
        if (!keep) {
            LOGI("CPR for player %d stopped", g_target);
            StopCpr(me, true);
        } else if (NowMs() - g_cprStart >= kCprMs) {
            SendOp(g_target, OP_DONE);
            LOGI("CPR for player %d done", g_target);
            StopCpr(me, false);
        }
        return;
    }
    if (!CanHelp(me)) return;
    float best = kReach;
    for (auto& [id, rp] : Players::Instance().All()) {
        if (!Wounded(id) || rp.ped->AreaCode() != me->AreaCode()) continue;
        float dist = (rp.ped->GetPosition() - me->GetPosition()).Length();
        if (dist < best) best = dist, g_candidate = id;
    }
}

// ---- this player wounded, every frame
void UpdateDowned(CPlayerPed* me) {
    if (g_wantDown) GoDown(me);
    if (!g_down) return;
    if (PlayerInfos()[0].playerState != 0) {  // CPlayerInfo::m_nPlayerState: wasted or busted after all
        EndDown(me);
        LOGI("Wounded: the game ended it (player state %d)", PlayerInfos()[0].playerState);
        return;
    }
    if (me->Health() <= 0.0f) {  // a script killed it (or it died some other way): the death goes on
        EndDown(me);
        LOGI("Wounded: died anyway");
        return;
    }
    Pad()->DisablePlayerControls |= kOwnControls;  // a script, the pause menu may have put the controls back
    me->PhysicalFlags() |= kProofBits;
    uint64_t now = NowMs();
    const RemotePlayer* rescuer = g_rescuer >= 0 ? Players::Instance().Get((uint8_t)g_rescuer) : nullptr;
    if (g_rescuer >= 0 && (!rescuer || !rescuer->hasState || !(rescuer->state.flags & OnFootState::F_REVIVING))) {
        if (!rescuer || now - rescuer->stateRecvMs > 3000) g_rescuer = -1;
    }
    if (g_rescuer >= 0) g_downEnd = std::max(g_downEnd, now + 1500);  // not while it is being revived
    if (me->InVehicle()) {  // a script put it in a car: no lying there
        Die(me, "put in a car");
        return;
    }
    if (now >= g_downEnd) {
        Die(me, "nobody came");
        return;
    }
    // giving up: the key held
    bool held = IsForeground() && !ui::TheChat().IsOpen() && (GetAsyncKeyState(Cfg().reviveKey) & 0x8000);
    g_holdMs = held ? (g_holdMs ? g_holdMs : now) : 0;
    if (g_holdMs && now - g_holdMs >= kGiveUpHoldMs) {
        Die(me, "gave up");
        return;
    }
    if (now - g_animCheckMs > 1000) {
        g_animCheckMs = now;
        if (!Cmd(0x0611).I(GetPedRef(me)).S(kDownAnim).Run()) LieDown(me);  // IS_CHAR_PLAYING_ANIM
    }
    Players::Instance().SetLocalDowned(true, (uint8_t)std::min<uint64_t>(255, (g_downEnd - now + 999) / 1000));
}

void HandleRevive(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    if (!r.Ok() || !g_down) return;
    if (op == OP_START) {
        g_rescuer = from;
        LOGI("Player %d gives this player CPR", from);
    } else if (op == OP_STOP) {
        if (g_rescuer == from) g_rescuer = -1;
    } else if (op == OP_DONE) {
        if (CPlayerPed* me = core::IsPlayerReady() ? FindPlayerPed(0) : nullptr) Revive(me, from);
    }
}

void ResetAll() {
    if (g_down || g_wantDown) EndDown(core::IsPlayerReady() ? FindPlayerPed(0) : nullptr);
    if (g_target >= 0) StopCpr(nullptr, false);
    g_candidate = -1;
}

class ReviveModule : public Module {
public:
    const char* Name() const override { return "Revive"; }
    void OnInstallHooks() override {
        mem::HookFn(0x4B3210, &HookWillKill, &g_origWillKill, "CPedDamageResponseCalculator::ComputeWillKillPed");
        Net().SetHandler(proto::G_REVIVE, &HandleRevive);
    }
    void OnSessionEnd() override { ResetAll(); }
    void OnGameShutdown() override {
        ResetAll();
        g_revives.clear();
    }
    void OnPeerLeave(uint8_t id) override {
        if (g_target == id) StopCpr(core::IsPlayerReady() ? FindPlayerPed(0) : nullptr, false);
        if (g_rescuer == id) g_rescuer = -1;
    }
    void OnPostProcess() override {
        if (!Net().Online() || !core::IsPlayerReady()) {
            if (g_down || g_target >= 0) ResetAll();
            return;
        }
        CPlayerPed* me = FindPlayerPed(0);
        if (!me) return;
        UpdateDowned(me);
        UpdateRescuer(me);
    }
};

ReviveModule g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

std::string ReviveText(std::string& hint) {
    if (!Net().Online() || !core::IsPlayerReady()) return {};
    if (g_down) {
        uint64_t now = NowMs();
        int secs = (int)((std::max(g_downEnd, now) - now + 999) / 1000);
        if (g_rescuer >= 0) {
            hint = Format(TR("%s откачивает вас", "%s is reviving you"), Name(g_rescuer).c_str());
        } else {
            hint = Format(TR("Ждите напарника · удерживайте %s, чтобы сдаться", "Wait for a partner · hold %s to give up"),
                          Key().c_str());
        }
        return Format(TR("Вы ранены · %d с", "You are wounded · %d s"), secs);
    }
    if (g_target >= 0) {
        int secs = (int)((kCprMs - std::min<uint64_t>(kCprMs, NowMs() - g_cprStart) + 999) / 1000);
        hint = Format(TR("%s — прервать", "%s — stop"), Key().c_str());
        return Format(TR("Откачиваете %s · %d с", "Reviving %s · %d s"), Name(g_target).c_str(), secs);
    }
    if (g_candidate >= 0) {
        hint = Format(TR("%s — откачать", "%s — revive"), Key().c_str());
        return Format(TR("%s ранен", "%s is wounded"), Name(g_candidate).c_str());
    }
    return {};
}

bool ReviveKeyPressed() {
    if (!Net().Online() || !core::IsPlayerReady()) return false;
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || g_down) return false;
    if (g_target >= 0) {
        StopCpr(me, true);
        return true;
    }
    if (g_candidate >= 0 && CanHelp(me)) {
        StartCpr(me, g_candidate);
        return g_target >= 0;
    }
    return false;
}

}  // namespace coop
