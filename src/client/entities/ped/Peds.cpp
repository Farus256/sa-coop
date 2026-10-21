// NPC synchronisation. Every game owns the peds it created (random population, mission peds); peds near
// the other player are announced and follow the owner on the other side as "mirrors": script-created
// peds with an empty decision maker and all proofs. What the owner's ped does is replayed with the
// game's own means so it looks native:
//  * walking/running   - "go to point" tasks towards the owner's position;
//  * aiming/shooting   - aim/shoot-at-coord tasks (gun pose, muzzle flash, sound, tracers);
//  * drive-bys         - the gang drive-by task (TASK_DRIVE_BY) at the same target, out of the mirror's car;
//  * entering/leaving  - enter/leave car tasks (door and seat animations);
//  * anything else     - the owner's current action animation (chat, phone, benches, fights, falls).
#include "entities/ped/Peds.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Anim.h"
#include "shared/game/Script.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

using namespace game;
using game::script::Cmd;

Peds& Peds::Instance() {
    static Peds p;
    return p;
}

namespace {
struct Reg {
    Reg() { core::RegisterModule(&Peds::Instance()); }
} g_reg;

// owned peds closer than "in" to a remote player are announced and removed there beyond "out" from all of them;
// vehicle occupants use the vehicle ranges so traffic is never seen without its driver
float SyncIn(CPed* p) { return p->InVehicle() ? 170.0f : 120.0f; }
float SyncOut(CPed* p) { return p->InVehicle() ? 220.0f : 150.0f; }
// Another game's ped on foot has a copy here while this game's player is near it (in a vehicle: while the vehicle has
// one): the players' games far apart keep each other's people as records only
constexpr float kMirrorIn = 135.0f, kMirrorOut = 180.0f;
constexpr int kSpecialFirst = 290, kSpecialLast = 299;
constexpr int kEmptyDecisionMaker = 65542;  // SET_CHAR_DECISION_MAKER template "Empty"
constexpr int TASK_ENTER_CAR_AS_PASSENGER = 700, TASK_ENTER_CAR_AS_DRIVER = 701, TASK_LEAVE_CAR = 704;
constexpr int TASK_SIMPLE_USE_GUN = 1017, TASK_SIMPLE_GANG_DRIVEBY = 1022;
constexpr uint64_t kCarTaskGraceMs = 6000;  // let enter/leave animations finish before warping
constexpr uint16_t kNoAnim = 0xFFFF;

Vec3 V(const CVector& v) { return {v.x, v.y, v.z}; }
CVector G(const Vec3& v) { return {v.x, v.y, v.z}; }

void WriteState(Writer& w, const PedSyncState& s) {
    w.U32(s.time);
    w.Vec(V(s.pos));
    w.VecQ(V(s.vel), 30.0f);
    w.Angle(s.heading);
    w.U8(s.moveState);
    w.U8(s.health);
    w.U8(s.armour);
    w.U8(s.weapon);
    w.U8(s.flags);
    w.U16(s.vehNetId);
    w.I8(s.seat);
    w.I8(s.area);
    if (s.flags & PedSyncState::F_AIMING) {
        w.VecQ(V(s.aim), 200.0f);
        w.U8(s.aimKind);
        if (s.aimKind) w.U16(s.aimId);
    }
    w.U16(s.animBlock);
    if (s.animBlock != kNoAnim) {
        w.U16(s.animIdx);
        w.U16(s.animFlags);
        w.F32(s.animTime);
    }
}

void ReadState(Reader& r, PedSyncState& s) {
    s.time = r.U32();
    s.pos = G(r.Vec());
    s.vel = G(r.VecQ(30.0f));
    s.heading = WrapAngle(r.Angle());
    s.moveState = r.U8();
    s.health = r.U8();
    s.armour = r.U8();
    s.weapon = r.U8();
    if (s.weapon > 46) s.weapon = 0;  // eWeaponType ends with the parachute: the weapon info past it is garbage
    s.flags = r.U8();
    s.vehNetId = r.U16();
    s.seat = r.I8();
    s.area = r.I8();
    if (s.flags & PedSyncState::F_AIMING) {
        s.aim = G(r.VecQ(200.0f));
        s.aimKind = r.U8();
        if (s.aimKind) s.aimId = r.U16();
    }
    s.animBlock = r.U16();
    if (s.animBlock != kNoAnim) {
        s.animIdx = r.U16();
        s.animFlags = r.U16();
        s.animTime = r.F32();
    }
}

using RequestSpecialFn = void(__cdecl*)(int, const char*, int);
RequestSpecialFn g_origRequestSpecial = nullptr;
// another character into the slot a player's copy wears (a cutscene, a mission, a mirror): CStreaming::
// RequestSpecialModel deletes the slot's model while that copy still has it on. It goes back to CJ first and gets its
// look again.
void MakeWay(int model, const char* name) {
    if (!name || !_stricmp(Peds::Instance().SpecialName(model).c_str(), name)) return;
    Peds::Instance().RespawnCopiesOf(model);
    for (auto& [id, other] : Players::Instance().All())
        if (RemotePlayer* rp = Players::Instance().Get(id); rp && rp->ped && rp->ped->ModelIndex() == model) {
            Players::SetModel(rp->ped, 0);
            rp->lookStale = true;
            LOGI("Player %d's special character makes way for %s", id, name);
        }
}

void __cdecl HookRequestSpecialModel(int model, const char* name, int flags) {
    MakeWay(model, name);
    if (name) Peds::Instance().RememberSpecialModel(model, name);
    g_origRequestSpecial(model, name, flags);
}

uint8_t LoadState(int model) { return Ref<uint8_t>(0x8E4CC0 + (uintptr_t)model * 0x14 + 0x10); }  // CStreaming::ms_aInfoForModel

uint8_t Clamp8(float v) { return (uint8_t)std::clamp(v, 0.0f, 255.0f); }

bool IsPedDead(CPed* p) {
    int st = p->PedState();
    return p->Health() <= 0.0f || st == 54 /*PEDSTATE_DIE*/ || st == 55 /*PEDSTATE_DEAD*/;
}

bool IsAlive(const NetPed& np) { return np.ped && IsPedValid(np.ped) && GetPedRef(np.ped) == np.handle; }

// CTaskManager::FindActiveTaskByType: searches the active primary chain and the secondary tasks
void* FindTask(CPed* ped, int type) {
    void* intel = ped->Intelligence();
    return intel ? CallMethod<0x681740, void*>((uint8_t*)intel + 4, type) : nullptr;
}

// The action animation the ped plays: anything but its own walk/run/idle set (CPed::m_nAnimGroup).
// Full-body animations while moving are turns and start/stop steps, also locomotion. A partial one (a punch, a guard)
// plays over a full-body one (the fight stance) and is the action while it lasts: the mirror keeps the stance under it.
void CaptureAnim(CPed* ped, PedSyncState& s) {
    int motion = Field<int32_t>(ped, 0x4D4);
    bool still = ped->MoveState() <= PEDMOVE_STILL;
    void* best = nullptr;
    bool bestPartial = false;
    for (void* a = anim::First(ped->RwObject()); a; a = anim::Next(a)) {
        if (anim::BlendAmount(a) < 0.3f || anim::BlendDelta(a) < 0.0f || anim::Group(a) == motion) continue;
        if (anim::Flags(a) & anim::F_INDESTRUCTIBLE) continue;  // the face's own (talk, gum: ANIMATION_FACIAL), not an action
        bool partial = (anim::Flags(a) & anim::F_PARTIAL) != 0;
        if (!partial && !still) continue;
        if (!best || partial > bestPartial || (partial == bestPartial && anim::BlendAmount(a) > anim::BlendAmount(best)))
            best = a, bestPartial = partial;
    }
    int block, idx;
    if (!best || !anim::Locate(anim::Hierarchy(best), block, idx)) return;
    s.animBlock = (uint16_t)block;
    s.animIdx = (uint16_t)idx;
    s.animFlags = anim::Flags(best);
    s.animTime = anim::CurrentTime(best);
}

void* FindPlaying(CPed* ped, int block, int idx) {
    int b, i;
    for (void* a = anim::First(ped->RwObject()); a; a = anim::Next(a))
        if (anim::Locate(anim::Hierarchy(a), b, i) && b == block && i == idx) return a;
    return nullptr;
}

// Every task a script gives takes one of 128 records (CPedScriptedTaskRecord::ms_scriptedTasks) until the ped is
// done with it. With all of them taken the game writes the new one in front of the array and crashes later. When
// that happens, finished records are let go first; failing that, one record goes (a script may then see that ped's
// task as over) and the log tells which commands hold them.
constexpr uintptr_t kTaskRecords = 0xC0B1E8;  // 20 bytes each: opcode, event, task, ped, status
using VacantSlotFn = int(__cdecl*)();
VacantSlotFn g_origVacantSlot = nullptr;
int __cdecl HookGetVacantSlot() {
    int i = g_origVacantSlot();
    if (i >= 0) return i;
    Call<0x6085E0>();  // CPedScriptedTaskRecord::Process
    if ((i = g_origVacantSlot()) >= 0) return i;
    static uint64_t loggedAt = 0;
    if (NowMs() - loggedAt > 10000) {
        loggedAt = NowMs();
        std::map<int, int> byOpcode;
        int mirrors = 0;
        for (int k = 0; k < 128; ++k) {
            uintptr_t r = kTaskRecords + (uintptr_t)k * 20;
            ++byOpcode[Ref<int32_t>(r)];
            if (CPed* p = Ref<CPed*>(r + 0x0C); p && Peds::Instance().Find(p) && Peds::Instance().Find(p)->owner != Net().MyId()) ++mirrors;
        }
        std::string s;
        for (auto& [op, n] : byOpcode) s += Format(" %04X:%d", op & 0xFFFF, n);
        LOGW("All 128 scripted task records are taken (%d by mirrored peds), by opcode:%s", mirrors, s.c_str());
    }
    CallMethod<0x608350>((void*)kTaskRecords);  // CPedScriptedTaskRecordData::Flush
    return 0;
}

// A task given by script that the ped has not taken up yet: its event waits (a ped this game does not process now,
// out where its collision is not loaded). Another one would only take another record.
bool TaskPending(CPed* ped) {
    for (int k = 0; k < 128; ++k) {
        uintptr_t r = kTaskRecords + (uintptr_t)k * 20;
        if (Ref<CPed*>(r + 0x0C) == ped && Ref<void*>(r + 0x04)) return true;  // m_ped, m_event
    }
    return false;
}
// A mirror replays its owner's full-body action (a fight stance) with no task behind it, so the game's own move
// animation (CPed::SetMoveAnim) blends its walk or idle over it whenever its move state changes: the stance faded
// and started over after every punch. While such an action holds the mirror, its move animation stays as it is.
using SetMoveAnimFn = void(__fastcall*)(CPed*, void*);
SetMoveAnimFn g_origSetMoveAnim = nullptr;
void __fastcall HookSetMoveAnim(CPed* ped, void* edx) {
    if (NetPed* np = Peds::Instance().Find(ped); np && np->owner != Net().MyId() && np->animFull) return;
    g_origSetMoveAnim(ped, edx);
}

// The frame update of a ped's animations takes the first one without looking (velocity extraction, 0x4D1750), so a
// ped left with none crashes the game: the game's own peds always keep their stance or walk under the rest. A mirror's
// action (a fight stance) fades out when its owner's ends, while its move animation was held back (above) and the
// game starts one only when the move state changes: games crashed half a second after a mirrored fight.
bool HasLastingBodyAnim(CPed* ped) {
    for (void* a = anim::First(ped->RwObject()); a; a = anim::Next(a))
        if (!(anim::Flags(a) & anim::F_PARTIAL) && anim::BlendDelta(a) >= 0.0f) return true;
    return false;
}
}  // namespace

// The people an interior brings (InteriorGroup_c::SetupPeds: a police station's officers, a shop's customers) keep
// area 0 inside: they count as in the interior of a player near them, ours or the other one's (the owner who walked
// out took them out with it: they vanished for the player who stayed inside, and never went to its game)
int8_t AreaOf(CPed* ped) {
    int8_t area = ped->AreaCode();
    if (area != 0) return area;
    CVector p = ped->GetPosition();
    CPlayerPed* me = FindPlayerPed(0);
    if (me && me->AreaCode() != 0 && (p - me->GetPosition()).Length() < 80.0f) return me->AreaCode();
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.hasState && rp.state.area != 0 && (p - rp.state.pos).Length() < 80.0f) return rp.state.area;
    return 0;
}

void Peds::RememberSpecialModel(int model, const char* name) {
    if (model >= kSpecialFirst && model <= kSpecialLast) m_specialNames[model] = name;
}

std::string Peds::SpecialName(int model) const {
    auto it = m_specialNames.find(model);
    return it == m_specialNames.end() ? std::string() : it->second;
}

int Peds::SpecialModelFor(const std::string& name) {
    if (name.empty()) return -1;
    for (int m = kSpecialFirst; m <= kSpecialLast; ++m)
        if (!_stricmp(SpecialName(m).c_str(), name.c_str()) && LoadState(m) != 0) return IsModelLoaded(m) ? m : -1;
    // a name this game's archives lack (another player's mods): RequestSpecialModel takes the garbage position and size
    // of a failed CDirectory::FindItem (0x409F76) and the streamer crashed on it
    void* dir = Ref<void*>(0x8E48D0);  // CStreaming::ms_pExtraObjectsDir
    if (!dir || !CallMethod<0x532450, void*>(dir, name.c_str())) return -1;  // CDirectory::FindItem
    for (int m = kSpecialLast; m >= kSpecialFirst; --m)
        if (LoadState(m) == 0) {  // nothing in it, nothing asked for
            g_origRequestSpecial(m, name.c_str(), 2);  // STREAMING_GAME_REQUIRED: kept while the copy wears it
            m_specialNames[m] = name;
            LOGI("Special character %s asked for in slot %d", name.c_str(), m);
            return -1;
        }
    return -1;
}

void Peds::OnInstallHooks() {
    mem::HookFn(0x409D10, &HookRequestSpecialModel, &g_origRequestSpecial, "CStreaming::RequestSpecialModel");
    mem::HookFn(0x608540, &HookGetVacantSlot, &g_origVacantSlot, "CPedScriptedTaskRecord::GetVacantSlot");
    mem::HookFn(0x5E4A00, &HookSetMoveAnim, &g_origSetMoveAnim, "CPed::SetMoveAnim");
    Net().SetHandler(proto::G_PED_CREATE, [](uint8_t from, Reader& r) { Instance().HandleCreate(from, r); });
    Net().SetHandler(proto::G_PED_REMOVE, [](uint8_t from, Reader& r) { Instance().HandleRemove(from, r); });
    Net().SetHandler(proto::G_PED_SYNC, [](uint8_t from, Reader& r) { Instance().HandleSync(from, r); });
    Net().SetHandler(proto::G_PED_OWNER, [](uint8_t from, Reader& r) { Instance().HandleOwner(from, r); });
    Net().SetHandler(proto::G_RESYNC, [](uint8_t from, Reader&) {
        if (!core::IsPlayerReady()) return;
        // its game started over: what it owned before is gone there
        Instance().DropMirrorsOf(from, false);
        Vehicles::Instance().ForgetOwnedBy(from);
        Vehicles::Instance().Reannounce(from);  // vehicles first: peds inside them need the vehicle
        Instance().Reannounce(from);
        core::PeerResynced(from);
    });
}

NetPed* Peds::Find(CPed* ped) {
    auto it = m_byPtr.find(ped);
    if (it == m_byPtr.end()) return nullptr;
    auto it2 = m_byId.find(it->second);
    return it2 == m_byId.end() ? nullptr : &it2->second;
}

CPed* Peds::FindOwned(uint16_t netId) {
    auto it = m_byId.find(netId);
    return it != m_byId.end() && it->second.owner == Net().MyId() && IsAlive(it->second) ? it->second.ped : nullptr;
}

int Peds::HandleOf(uint16_t netId) {
    auto it = m_byId.find(netId);
    return it != m_byId.end() && IsAlive(it->second) ? it->second.handle : -1;
}

PedSyncState Peds::Capture(NetPed& np) {
    CPed* ped = np.ped;
    uint64_t now = NowMs();
    PedSyncState s;
    s.time = (uint32_t)now;
    s.pos = ped->GetPosition();
    s.vel = ped->MoveSpeed() * 50.0f;
    s.heading = ped->CurrentRotation();
    s.moveState = (uint8_t)std::clamp(ped->MoveState(), 0, 7);
    s.health = Clamp8(ped->Health());
    s.armour = Clamp8(ped->Armour());
    s.weapon = (uint8_t)ped->ActiveWeapon().type;
    s.area = AreaOf(ped);
    s.flags = (IsPedDead(ped) ? PedSyncState::F_DEAD : 0) | (ped->IsDucking() ? PedSyncState::F_DUCKING : 0);

    // guns: shots show up as the clip emptying (the gun task fires on its own schedule), on foot and out of a car
    CWeapon& w = ped->ActiveWeapon();
    if (w.type != np.lastWeapon) np.lastClip = -1;
    if (np.lastClip >= 0 && w.ammoInClip < np.lastClip) np.lastShotMs = now;
    np.lastClip = w.ammoInClip;
    np.lastWeapon = (uint8_t)w.type;
    auto aimAt = [&](CEntity* target, CVector tp) {
        if (!target && tp.Length() < 1.0f)
            tp = s.pos + CVector{-std::sin(s.heading) * 10.0f, std::cos(s.heading) * 10.0f, 0.0f};
        s.flags |= PedSyncState::F_AIMING;
        s.aim = tp - s.pos;
        if (target && target->IsPed()) {  // a ped target is tracked by the mirror's own task
            auto* tped = (CPed*)target;
            if (tped == FindPlayerPed(0)) {
                s.aimKind = 1;
                s.aimId = Net().MyId();
            } else if (RemotePlayer* rp = Players::Instance().FindByPed(tped)) {
                s.aimKind = 1;
                s.aimId = rp->id;
            } else if (NetPed* tn = Find(tped)) {
                s.aimKind = 2;
                s.aimId = tn->netId;
            }
        }
        if (now - np.lastShotMs < 600) s.flags |= PedSyncState::F_FIRING;
    };

    if (CVehicle* v = ped->VehicleIfIn()) {
        if (NetVehicle* nv = Vehicles::Instance().EnsureNetworked(v)) {
            s.flags |= PedSyncState::F_IN_VEHICLE;
            s.vehNetId = nv->netId;
            s.seat = (int8_t)PassengerSeatOf(v, ped);
            if (FindTask(ped, TASK_LEAVE_CAR)) s.flags |= PedSyncState::F_LEAVING;
            // a drive-by: CTaskSimpleGangDriveBy::m_pTargetEntity, else m_vecCoords (an offset from a target)
            if (void* db = FindTask(ped, TASK_SIMPLE_GANG_DRIVEBY)) {
                auto* target = Field<CEntity*>(db, 0x34);
                aimAt(target, target ? target->GetPosition() : Field<CVector>(db, 0x38));
            }
        }
        return s;
    }

    void* enter = FindTask(ped, TASK_ENTER_CAR_AS_DRIVER);
    bool asDriver = enter != nullptr;
    if (!enter) enter = FindTask(ped, TASK_ENTER_CAR_AS_PASSENGER);
    if (enter) {
        auto* target = Field<CVehicle*>(enter, 0x0C);  // CTaskComplexEnterCar::m_Car
        if (target && IsVehicleValid(target) && IsSyncableVehicle(target))
            if (NetVehicle* nv = Vehicles::Instance().EnsureNetworked(target)) {
                s.flags |= PedSyncState::F_ENTERING;
                s.vehNetId = nv->netId;
                // m_TargetDoor once at the door, before that the door the go-to-door task was given (m_TargetSeat)
                int door = Field<int32_t>(enter, 0x14) ? Field<int32_t>(enter, 0x14) : Field<int32_t>(enter, 0x1C);
                s.seat = asDriver ? -1 : (int8_t)SeatOfTargetDoor(door);
            }
        return s;
    }

    if (void* gun = FindTask(ped, TASK_SIMPLE_USE_GUN)) {
        auto* target = Field<CEntity*>(gun, 0x1C);  // CTaskSimpleUseGun::m_pTarget, else m_vecTarget
        aimAt(target, target ? target->GetPosition() : Field<CVector>(gun, 0x20));
        return s;
    }
    CaptureAnim(ped, s);
    return s;
}

void Peds::Announce(NetPed& np, proto::PlayerMask to) {
    PedSyncState s = Capture(np);
    Writer w(proto::G_PED_CREATE);
    w.U16(np.netId);
    w.U16(np.model);
    w.U8(np.pedType);
    w.Str(np.special, 16);
    WriteState(w, s);
    Net().SendTo(to, proto::CH_RELIABLE, w);
    np.lastSendMs = NowMs();
}

void Peds::Reannounce(uint8_t to) {
    for (auto& [id, np] : m_byId)
        if (np.owner == Net().MyId() && IsAlive(np)) Announce(np, proto::Bit(to));
}

// The players that have a copy of the ped or are about to: those near it, and in a vehicle those its copy is made for
proto::PlayerMask Peds::StateTargets(NetPed& np) {
    CPed* ped = np.ped;
    proto::PlayerMask mask = PlayersNear(ped->GetPosition(), AreaOf(ped), SyncOut(ped) + 40.0f, false);
    if (CVehicle* v = ped->VehicleIfIn())
        if (NetVehicle* nv = Vehicles::Instance().Find(v)) mask |= Vehicles::Instance().Riders(nv->netId);
    return mask;
}

void Peds::SendState(NetPed& np) {
    proto::PlayerMask to = StateTargets(np);
    np.lastSendMs = NowMs();
    if (!to) return;
    Writer w(proto::G_PED_SYNC);
    w.U16(np.netId);
    WriteState(w, Capture(np));
    Net().SendTo(to, proto::CH_STATE, w);
}

void Peds::RespawnCopiesOf(int model) {
    for (auto& [id, np] : m_byId) {
        if (np.owner == Net().MyId() || !IsAlive(np) || np.ped->ModelIndex() != model) continue;
        Cmd(0x009B).I(np.handle).Run();  // DELETE_CHAR: SpawnMirror makes it again
        m_byPtr.erase(np.ped);
        np.ped = nullptr;
        np.handle = -1;
        LOGI("Mirror ped %04X makes way for another special character", id);
    }
}

void Peds::Forget(uint16_t netId, bool deletePed) {
    auto it = m_byId.find(netId);
    if (it == m_byId.end()) return;
    NetPed& np = it->second;
    if (deletePed && IsAlive(np)) Cmd(0x009B).I(np.handle).Run();  // DELETE_CHAR
    m_byPtr.erase(np.ped);
    m_byId.erase(it);
}

void Peds::ScanOwned() {
    CPoolHeader* pool = PedPool();
    if (!pool) return;
    CPlayerPed* me = FindPlayerPed(0);
    uint8_t myId = Net().MyId();
    uint64_t now = NowMs();

    // peds this game would have dropped (CPopulation::ManagePed) while the partner is nearer go to the partner's game
    for (auto& [ped, to] : m_giveAway)
        if (NetPed* np = IsPedValid(ped) ? Find(ped) : nullptr; np && np->owner == myId && IsAlive(*np)) GiveAway(np->netId, to);
    m_giveAway.clear();

    // owned peds that vanished or went out of the partner's range
    std::vector<uint16_t> gone;
    for (auto& [id, np] : m_byId)
        if (np.owner == myId && (!IsAlive(np) || RemoteDistance(np.ped->GetPosition(), AreaOf(np.ped)) > SyncOut(np.ped)))
            gone.push_back(id);
    for (uint16_t id : gone) {
        Writer w(proto::G_PED_REMOVE);
        w.U16(id);
        Net().Send(proto::CH_RELIABLE, w);
        Forget(id, false);
    }

    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* ped = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
        if (ped == me || ped->IsPlayer() || Players::Instance().FindByPed(ped)) continue;
        NetPed* np = Find(ped);
        if (np && np->owner != myId) continue;  // mirror
        float d = RemoteDistance(ped->GetPosition(), AreaOf(ped));
        if (!np) {
            if (d > SyncIn(ped)) continue;
            if (CVehicle* v = ped->VehicleIfIn(); v && !IsSyncableVehicle(v)) continue;
            uint16_t id;
            do {
                id = proto::EntityId(myId, m_nextId++);
            } while (m_byId.count(id));
            NetPed& n = m_byId[id];
            n.netId = id;
            n.ped = ped;
            n.handle = GetPedRef(ped);
            n.owner = myId;
            n.model = ped->ModelIndex();
            n.pedType = (uint8_t)ped->PedType();
            if (auto it = m_specialNames.find(n.model); it != m_specialNames.end()) n.special = it->second;
            m_byPtr[ped] = id;
            Announce(n);
            continue;
        }
        uint64_t interval = d < 40.0f ? 100 : (d < 80.0f ? 200 : 400);
        if (now - np->lastSendMs >= interval) SendState(*np);
    }
}

CPed* Peds::SpawnMirror(NetPed& np) {
    const PedSyncState& st = np.state;
    int model = np.model;
    if (model >= kSpecialFirst && model <= kSpecialLast) {
        if (np.special.empty()) return nullptr;
        // in whichever slot holds that name here, or a free one: the owner's slot can belong to someone here (a
        // mission's person, another copy), and RequestSpecialModel took the model from under them
        model = SpecialModelFor(np.special);
        if (model < 0) return nullptr;
    } else if (!IsPedModel(model)) {
        return nullptr;
    }
    if (!IsModelLoaded(model)) {
        // streamed in the background, the spawn is retried (a blocking load per ped stalls the frame)
        RequestModel(model, 0);
        return nullptr;
    }
    if (!HasRoom(PedPool(), kKeepPeds, "ped")) return nullptr;
    // mirrors are civilians whatever the owner's type: cop/gang/emergency classes add their own AI and
    // the cop class even replaces the model
    int pedType = np.pedType == PED_TYPE_CIVFEMALE ? PED_TYPE_CIVFEMALE : PED_TYPE_CIVMALE;
    int handle = -1;
    if (st.flags & PedSyncState::F_IN_VEHICLE) {
        NetVehicle* nv = Vehicles::Instance().Find(st.vehNetId);
        if (!nv || !Vehicles::Alive(*nv)) return nullptr;  // wait for the vehicle
        if (st.seat < 0) {
            if (nv->veh->Driver()) return nullptr;
            Cmd(0x0129).I(nv->handle).I(pedType).I(model).Out(&handle).Run();  // CREATE_CHAR_INSIDE_CAR
        } else {
            if (st.seat >= 8 || nv->veh->Passengers()[st.seat]) return nullptr;
            Cmd(0x01C8).I(nv->handle).I(pedType).I(model).I(st.seat).Out(&handle).Run();  // CREATE_CHAR_AS_PASSENGER
        }
    } else {
        Cmd(0x009A).I(pedType).I(model).F(st.pos.x).F(st.pos.y).F(st.pos.z - 1.0f).Out(&handle).Run();  // CREATE_CHAR
    }
    CPed* ped = GetPed(handle);
    if (!ped) return nullptr;
    Cmd(0x060B).I(handle).I(kEmptyDecisionMaker).Run();  // SET_CHAR_DECISION_MAKER: reacts to nothing by itself
    Cmd(0x02AB).I(handle).I(1).I(1).I(1).I(1).I(1).Run();  // SET_CHAR_PROOFS: damage is decided by the owner
    Cmd(0x087E).I(handle).I(0).Run();                      // SET_CHAR_DROPS_WEAPONS_WHEN_DEAD false
    ped->CurrentRotation() = ped->AimingRotation() = st.heading;
    ped->AreaCode() = st.area;
    np.ped = ped;
    np.handle = handle;
    m_byPtr[ped] = np.netId;
    return ped;
}

void Peds::HandleCreate(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    uint16_t model = r.U16();
    uint8_t pedType = r.U8();
    std::string special = r.Str(16);
    PedSyncState st;
    ReadState(r, st);
    if (!r.Ok() || from == Net().MyId() || !core::IsPlayerReady() || m_byId.count(id)) return;
    NetPed& np = m_byId[id];
    np.netId = id;
    np.owner = from;
    np.model = model;
    np.pedType = pedType;
    np.special = special;
    np.state = st;
    np.hasState = true;
    np.recvMs = NowMs();
    if (WantMirror(np)) SpawnMirror(np);  // may fail for now: retried from ApplyMirror
}

void Peds::HandleRemove(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    if (!r.Ok()) return;
    auto it = m_byId.find(id);
    if (it == m_byId.end() || it->second.owner != from) return;
    Forget(id, true);
}

bool Peds::CanGiveAwayType(const NetPed& np) {
    return np.pedType != PED_TYPE_COP && np.pedType != PED_TYPE_MEDIC && np.pedType != PED_TYPE_FIREMAN;
}

// A passer-by of this game: alive, on foot, not a script's (a mission's ped stays with its script), not the player's
// recruit and not in view (CPed::CanBeDeleted and CEntity::GetIsOnScreen, as the engine's own removal)
bool Peds::QueueGiveAway(CPed* ped, uint8_t to) {
    NetPed* np = Find(ped);
    if (!np || np->owner != Net().MyId() || ped->InVehicle() || ped->CreatedBy() != 1 /* PED_GAME */ || IsPedDead(ped) ||
        !CanGiveAwayType(*np) || !CallMethod<0x5DF100, bool>(ped) || CallMethod<0x534540, bool>(ped))
        return false;
    m_giveAway[ped] = to;
    return true;
}

void Peds::GiveAway(uint16_t netId, uint8_t to) {
    Writer w(proto::G_PED_OWNER);
    w.U16(netId);
    w.U8(to);
    Net().Send(proto::CH_RELIABLE, w);
    LOGD("Ped %04X handed over to player %d", netId, to);
    Forget(netId, true);
}

// The owner handed its ped over: the mirror is this game's own from now on, an ordinary ped (its own decision maker
// and a wander task, CTheScripts::CleanUpThisPed); in a car handed over with it, it drives on.
void Peds::HandleOwner(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    uint8_t to = r.U8();
    if (!r.Ok()) return;
    auto it = m_byId.find(id);
    if (it == m_byId.end() || it->second.owner != from) return;
    NetPed& np = it->second;
    if (to != Net().MyId() || !IsAlive(np) || IsPedDead(np.ped)) return Forget(id, true);
    Adopt(np);
    Forget(id, false);
    LOGD("Ped %04X taken over from player %d", id, from);
}

// a mirror becomes this game's own ped: it can be hurt, drops its loot, and the population manages it
void Peds::Adopt(NetPed& np) {
    Cmd(0x02AB).I(np.handle).I(0).I(0).I(0).I(0).I(0).Run();  // SET_CHAR_PROOFS off
    Cmd(0x087E).I(np.handle).I(1).Run();                      // SET_CHAR_DROPS_WEAPONS_WHEN_DEAD
    Cmd(0x01C2).I(np.handle).Run();                           // MARK_CHAR_AS_NO_LONGER_NEEDED
    // A copy is a civilian that reacts to nothing (SpawnMirror): ours now, it is what its owner had again and acts by its
    // kind (a gang member stood still under fire). Cops, medics and firemen are other classes: they stay civilians.
    if (np.pedType > PED_TYPE_COP && np.pedType != PED_TYPE_MEDIC && np.pedType != PED_TYPE_FIREMAN && np.pedType <= 31)
        np.ped->PedType() = np.pedType;
    CallMethod<0x5E06E0>(np.ped);  // CPed::SetPedDefaultDecisionMaker: the one of its stats, now that no script holds it
}

void Peds::HandleSync(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    PedSyncState st;
    ReadState(r, st);
    if (!r.Ok()) return;
    auto it = m_byId.find(id);
    if (it == m_byId.end() || it->second.owner != from) return;
    NetPed& np = it->second;
    if ((int32_t)(st.time - np.state.time) < 0) return;  // reordered
    np.state = st;
    np.recvMs = NowMs();
}

bool Peds::WantMirror(const NetPed& np) const {
    const PedSyncState& st = np.state;
    if (st.flags & PedSyncState::F_IN_VEHICLE) {  // with its vehicle's copy
        NetVehicle* nv = Vehicles::Instance().Find(st.vehNetId);
        return nv && nv->veh;
    }
    CPlayerPed* me = FindPlayerPed(0);
    return me && (st.pos - me->GetPosition()).Length() < (np.ped ? kMirrorOut : kMirrorIn);
}

// far from this game's player: the copy goes, the record stays (the owner's states keep it up to date)
void Peds::Unspawn(NetPed& np) {
    if (IsAlive(np)) Cmd(0x009B).I(np.handle).Run();  // DELETE_CHAR
    m_byPtr.erase(np.ped);
    NetPed fresh;  // the mirror side starts over when it is made again
    fresh.netId = np.netId;
    fresh.owner = np.owner;
    fresh.model = np.model;
    fresh.pedType = np.pedType;
    fresh.special = np.special;
    fresh.hasState = np.hasState;
    fresh.state = np.state;
    fresh.recvMs = np.recvMs;
    np = fresh;
}

void Peds::ApplyMirror(NetPed& np) {
    const PedSyncState& st = np.state;
    uint64_t now = NowMs();
    if (!np.ped) {
        if (now - np.lastTaskMs >= 100 && WantMirror(np)) {
            np.lastTaskMs = now;
            SpawnMirror(np);
        }
        return;
    }
    CPed* ped = np.ped;
    if (!IsAlive(np)) {  // deleted by the game (should not happen to mission peds): spawn again
        m_byPtr.erase(ped);
        np.ped = nullptr;
        return;
    }
    if (!WantMirror(np)) {
        Unspawn(np);
        return;
    }
    ped->AreaCode() = st.area;
    if (st.flags & PedSyncState::F_DEAD) {
        if (!np.deadApplied) {
            np.deadApplied = true;
            Cmd(0x02AB).I(np.handle).I(0).I(0).I(0).I(0).I(0).Run();  // proofs off so it can die
            if (now - np.lastHitMs < 2000) {  // our player killed it: the loot falls here (its owner drops none)
                Cmd(0x087E).I(np.handle).I(1).Run();  // SET_CHAR_DROPS_WEAPONS_WHEN_DEAD
                bool payless = np.pedType == PED_TYPE_COP || np.pedType == PED_TYPE_MEDIC || np.pedType == PED_TYPE_FIREMAN;
                // SET_CHAR_MONEY: a script ped drops only the money a script gave it
                Cmd(0x03FE).I(np.handle).I(payless ? 0 : ped->Money()).Run();
            }
            Cmd(0x05BE).I(np.handle).Run();                            // TASK_DIE
        }
        return;
    }
    ped->Health() = std::max<float>(1.0f, st.health);
    ped->Armour() = st.armour;
    if (CWeapon& w = ped->ActiveWeapon(); w.type == np.lastWeapon && w.ammoInClip < np.lastClip) ++m_mirrorShots;
    np.lastClip = ped->ActiveWeapon().ammoInClip;
    np.lastWeapon = (uint8_t)ped->ActiveWeapon().type;

    if (ped->ActiveWeapon().type != st.weapon && now - np.weaponMs > 1000) {
        np.weaponMs = now;
        if (st.weapon != 0) {
            int wmodel = -1;
            Cmd(0x0781).I(st.weapon).Out(&wmodel).Run();  // GET_WEAPONTYPE_MODEL
            if (wmodel > 0 && !IsModelLoaded(wmodel)) {
                RequestModel(wmodel, 0);
                return;
            }
            Cmd(0x01B2).I(np.handle).I(st.weapon).I(500).Run();  // GIVE_WEAPON_TO_CHAR
        }
        Cmd(0x01B9).I(np.handle).I(st.weapon).Run();  // SET_CURRENT_CHAR_WEAPON
    }

    // the copy here of what the owner's ped aims at (the real player when it is ours), -1: the point st.pos + st.aim
    auto aimHandle = [&]() -> int {
        if (st.aimKind == 1) {
            CPed* p = st.aimId == Net().MyId() ? FindPlayerPed(0) : nullptr;
            if (const RemotePlayer* rp = Players::Instance().Get((uint8_t)st.aimId); rp && st.aimId != Net().MyId()) p = rp->ped;
            return p ? GetPedRef(p) : -1;
        }
        if (st.aimKind == 2)
            if (auto it = m_byId.find(st.aimId); it != m_byId.end() && IsAlive(it->second)) return it->second.handle;
        return -1;
    };

    // ---- vehicles: the game's own enter/leave tasks, warps only as a fallback
    if (st.flags & PedSyncState::F_IN_VEHICLE) {
        NetVehicle* nv = Vehicles::Instance().Find(st.vehNetId);
        if (!nv || !Vehicles::Alive(*nv)) return;
        bool inside = ped->InVehicle() && ped->Vehicle() == nv->veh;
        if (inside && (st.flags & PedSyncState::F_LEAVING) && !np.leaveTaskMs) {
            Cmd(0x05CD).I(np.handle).I(nv->handle).Run();  // TASK_LEAVE_CAR
            np.leaveTaskMs = now;
        }
        int seat = inside ? PassengerSeatOf(nv->veh, ped) : -2;
        if (seat == st.seat) {
            if (np.leaveTaskMs) return;  // getting out: TASK_LEAVE_CAR ends a drive-by by itself
            // a drive-by: the game's gang drive-by task leans out, aims and fires at the owner's target; its shots hurt
            // what this game owns, the player too, as a mirror's gunfire on foot does
            bool aiming = (st.flags & PedSyncState::F_AIMING) && st.weapon != 0;
            int target = aiming ? aimHandle() : -1;
            CVector at = st.pos + st.aim;
            int rate = (st.flags & PedSyncState::F_FIRING) ? 90 : 0;  // m_nFrequencyPercentage: 0 leans out and aims only
            if (void* db = FindTask(ped, TASK_SIMPLE_GANG_DRIVEBY)) {
                if (!aiming || target != np.gunTargetHandle) {
                    CallMethod<0x601420>(ped->Intelligence(), true, false);  // CPedIntelligence::ClearTasks: seated again
                } else {
                    int8_t& freq = Field<int8_t>(db, 0x13);
                    if (rate && !freq) Field<int16_t>(db, 0x16) = -1;  // m_nAttackTimer: drawn again (rate 0 drew up to 10 s)
                    freq = (int8_t)rate;
                    if (target < 0) Field<CVector>(db, 0x38) = at;  // m_vecCoords: the point follows the owner's
                }
            } else if (aiming && ped->ActiveWeapon().type == st.weapon && now - np.gunTaskMs > 1000 && !TaskPending(ped)) {
                if (target >= 0) at = CVector{};  // with a target char the point is an offset from it
                // TASK_DRIVE_BY char, target char, target car, x, y, z, abort radius (the owner ends it), style
                // AI_ALL_DIRN, right-hand seat (the seat decides), fire rate
                Cmd(0x0713).I(np.handle).I(target).I(-1).F(at.x).F(at.y).F(at.z).F(1000.0f).I(8).I(0).I(rate).Run();
                np.gunTargetHandle = target;
                np.gunTaskMs = now;
            }
            return;
        }
        if (now - np.lastTaskMs < 1000) return;
        if (!inside && np.leaveTaskMs && now - np.leaveTaskMs < kCarTaskGraceMs) return;  // getting out like the owner
        if (!inside && np.enterVeh == st.vehNetId && now - np.enterTaskMs < kCarTaskGraceMs) return;  // still getting in
        np.lastTaskMs = now;
        if (ped->InVehicle()) {
            CVector p = ped->GetPosition();
            Cmd(0x0362).I(np.handle).F(p.x).F(p.y).F(p.z).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
        }
        if (st.seat < 0) {
            if (!nv->veh->Driver()) Cmd(0x036A).I(np.handle).I(nv->handle).Run();  // WARP_CHAR_INTO_CAR
        } else if (SeatOrAny(nv->veh, st.seat) >= 0 && !nv->veh->Passengers()[st.seat]) {
            Cmd(0x0430).I(np.handle).I(nv->handle).I(st.seat).Run();  // WARP_CHAR_INTO_CAR_AS_PASSENGER
        }
        return;
    }
    if (ped->InVehicle()) {
        if (!np.leaveTaskMs) {
            Cmd(0x05CD).I(np.handle).I(GetVehicleRef(ped->Vehicle())).Run();  // TASK_LEAVE_CAR
            np.leaveTaskMs = now;
        } else if (now - np.leaveTaskMs > kCarTaskGraceMs) {
            Cmd(0x0362).I(np.handle).F(st.pos.x).F(st.pos.y).F(st.pos.z - 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD (feet)
            np.leaveTaskMs = now;
        }
        return;
    }
    np.leaveTaskMs = 0;
    if (st.flags & PedSyncState::F_ENTERING) {
        NetVehicle* nv = Vehicles::Instance().Find(st.vehNetId);
        if (nv && Vehicles::Alive(*nv) && np.enterVeh != st.vehNetId) {
            if (st.seat < 0) Cmd(0x05CB).I(np.handle).I(nv->handle).I(10000).Run();  // TASK_ENTER_CAR_AS_DRIVER
            else Cmd(0x05CA).I(np.handle).I(nv->handle).I(10000).I(SeatOrAny(nv->veh, st.seat)).Run();  // TASK_ENTER_CAR_AS_PASSENGER
            np.enterVeh = st.vehNetId;
            np.enterTaskMs = now;
            np.lastTaskMove = 0;
        }
        if (np.enterVeh == st.vehNetId) return;  // the task walks to the car
    } else if (now - np.enterTaskMs > kCarTaskGraceMs) {
        np.enterVeh = 0;
    }

    // ---- on foot
    float age = std::min(0.5f, (float)(now - np.recvMs) / 1000.0f);
    CVector target = st.pos + st.vel * age;
    CVector cur = ped->GetPosition();
    CVector err = target - cur;
    float d = err.Length2D();
    if (d > 6.0f || std::fabs(err.z) > 4.0f) {
        Cmd(0x00A1).I(np.handle).F(target.x).F(target.y).F(target.z - 1.0f).Run();  // SET_CHAR_COORDINATES
        np.lastTaskMove = 0;
        np.gunMode = 0;
        return;
    }

    // guns
    if ((st.flags & PedSyncState::F_AIMING) && st.weapon != 0) {
        CVector aimAt = st.pos + st.aim;
        uint8_t mode = (st.flags & PedSyncState::F_FIRING) ? 2 : 1;
        int targetHandle = aimHandle();
        bool moved = targetHandle < 0 && (aimAt - np.gunTarget).Length() > 1.5f;
        if (mode != np.gunMode || targetHandle != np.gunTargetHandle || moved || now - np.gunTaskMs > 1500) {
            if (targetHandle >= 0)  // TASK_SHOOT_AT_CHAR / TASK_AIM_GUN_AT_CHAR: follows a moving target itself
                Cmd(mode == 2 ? 0x074D : 0x0635).I(np.handle).I(targetHandle).I(2000).Run();
            else  // TASK_SHOOT_AT_COORD / TASK_AIM_GUN_AT_COORD
                Cmd(mode == 2 ? 0x0668 : 0x0667).I(np.handle).F(aimAt.x).F(aimAt.y).F(aimAt.z).I(2000).Run();
            np.gunMode = mode;
            np.gunTargetHandle = targetHandle;
            np.gunTarget = aimAt;
            np.gunTaskMs = now;
        }
        np.lastTaskMove = 0;
        if (d > 0.25f) SetEntityPosition(ped, cur + CVector{err.x * 0.2f, err.y * 0.2f, 0});
        return;
    }
    if (np.gunMode) {
        np.gunMode = 0;
        np.lastTaskMove = 0;
    }

    // action animation (a restart of the same one shows up as its time going back)
    // a looped one going back is its next lap, not a restart (a fight stance would start over every lap)
    bool restarted = st.animBlock != kNoAnim && !(st.animFlags & anim::F_LOOPED) && st.animTime + 0.25f < np.animTime;
    np.animTime = st.animTime;
    if (st.animBlock != np.animBlock || st.animIdx != np.animIdx || restarted) {
        bool partial = st.animBlock != kNoAnim && (st.animFlags & anim::F_PARTIAL);
        bool sameBase = st.animBlock == np.baseBlock && st.animIdx == np.baseIdx;
        bool wasOverlay = np.animBlock != kNoAnim && (np.animBlock != np.baseBlock || np.animIdx != np.baseIdx);
        if (wasOverlay)  // a punch or a guard that ended or changed
            if (void* a = FindPlaying(ped, np.animBlock, np.animIdx)) anim::FadeOut(a);
        if (np.baseBlock != kNoAnim && !partial && (!sameBase || restarted)) {  // the full-body one changes or ends
            if (void* a = FindPlaying(ped, np.baseBlock, np.baseIdx)) anim::FadeOut(a);
            np.baseBlock = kNoAnim;
        }
        np.animBlock = kNoAnim;
        if (st.animBlock != kNoAnim) {
            // the stance an overlay played over is still there: it carries on instead of starting over
            void* a = np.baseBlock != kNoAnim && sameBase ? FindPlaying(ped, np.baseBlock, np.baseIdx) : nullptr;
            if (a) {
                anim::BlendDelta(a) = 4.0f;
            } else if (void* h = anim::Resolve(st.animBlock, st.animIdx)) {  // not loaded yet: retried next frame
                if ((a = anim::Blend(ped->RwObject(), h, st.animFlags, 4.0f))) {
                    float total = anim::TotalTime(h), t = st.animTime + age;
                    LOGD("Mirror %04X plays %d/%d at %.2f of %.2f s (flags %04X%s)", np.netId, st.animBlock, st.animIdx, t, total,
                         st.animFlags, restarted ? ", again" : "");
                    anim::SetCurrentTime(a, (st.animFlags & anim::F_LOOPED) && total > 0 ? std::fmod(t, total) : std::min(t, total));
                    np.lastTaskMove = 0;
                }
            }
            if (a) {
                np.animBlock = st.animBlock;
                np.animIdx = st.animIdx;
                if (!partial) np.baseBlock = st.animBlock, np.baseIdx = st.animIdx;
            }
        }
        np.animFull = np.baseBlock != kNoAnim;  // a full-body action holds it, also under an overlay
    }
    if (np.animFull) {  // a full-body action plays: hold the position without locomotion tasks
        if (d > 0.25f) SetEntityPosition(ped, cur + CVector{err.x * 0.2f, err.y * 0.2f, 0});
        ped->CurrentRotation() = ped->AimingRotation() = LerpAngle(ped->CurrentRotation(), st.heading, 0.2f);
        return;
    }

    // walking/running with the normal animations towards the owner's position
    float speed = st.vel.Length2D();
    if (speed > 0.4f || d > 1.2f) {
        uint8_t move = st.moveState >= 7 ? 7 : (st.moveState >= 5 || speed > 3.5f ? 6 : 4);
        CVector goal = target + st.vel * 0.6f;  // aim slightly ahead so the walk does not stop between updates
        bool due = now - np.lastTaskMs > 400 || move != np.lastTaskMove || (goal - np.lastTaskTarget).Length2D() > 1.5f;
        if (due && !TaskPending(ped)) {
            Cmd(0x05D3).I(np.handle).F(goal.x).F(goal.y).F(goal.z).I(move).I(2000).Run();  // TASK_GO_STRAIGHT_TO_COORD
            np.lastTaskMs = now;
            np.lastTaskTarget = goal;
            np.lastTaskMove = move;
        }
        // the task alone leaves it behind (a waiting task, something in its way only here, a straight line)
        if (d > 1.5f) SetEntityPosition(ped, cur + CVector{err.x * 0.05f, err.y * 0.05f, 0});
    } else {
        if (np.lastTaskMove != 1 && !TaskPending(ped)) {
            Cmd(0x05BA).I(np.handle).I(1000).Run();  // TASK_STAND_STILL
            np.lastTaskMove = 1;
        }
        if (d > 0.25f) SetEntityPosition(ped, cur + CVector{err.x * 0.2f, err.y * 0.2f, 0});
        ped->CurrentRotation() = ped->AimingRotation() = LerpAngle(ped->CurrentRotation(), st.heading, 0.2f);
    }
}

void Peds::OnPreProcess() {
    bool ready = Net().Online() && core::IsPlayerReady();
    if (ready && !m_wasReady) {
        // Our game just became playable: whatever the partner announced before is missing here. Sent before any
        // OnPostProcess (the vehicles' scan comes first there): the partner's G_RESYNC handler drops all we announced
        // before it, and the cars of the new world announced in the same frame were gone there for good.
        Writer w(proto::G_RESYNC);
        Net().Send(proto::CH_RELIABLE, w);
    }
    m_wasReady = ready;
    if (!ready) return;
    for (auto& [id, np] : m_byId) {
        if (np.owner == Net().MyId()) continue;
        ApplyMirror(np);
        CPed* ped = np.ped;
        if (!ped || !IsAlive(np) || !ped->RwObject() || ped->InVehicle() || IsPedDead(ped) || HasLastingBodyAnim(ped)) continue;
        // its stance comes back, as the game would give it (see HasLastingBodyAnim)
        Call<0x4D4610, void*>(ped->RwObject(), Field<int32_t>(ped, 0x4D4), 3 /* ANIM_ID_IDLE */, 4.0f);  // CAnimManager::BlendAnimation
        np.baseBlock = kNoAnim;
        np.animFull = false;
        LOGD("Mirror %04X had no lasting animation: its stance is back", np.netId);
    }
}

void Peds::OnPostProcess() {
    bool ready = Net().Online() && core::IsPlayerReady();
    if (!ready || NowMs() - m_lastScan < 50) return;
    m_lastScan = NowMs();
    ScanOwned();
    if (NowMs() - m_lastStatsMs >= 5000) {
        m_lastStatsMs = NowMs();
        int owned = 0, mirrors = 0, spawned = 0, acting = 0, aiming = 0, gunTasks = 0;
        for (auto& [id, np] : m_byId) {
            if (np.owner == Net().MyId()) {
                ++owned;
                continue;
            }
            ++mirrors;
            spawned += np.ped != nullptr;
            acting += np.animBlock != kNoAnim;
            aiming += np.gunMode != 0;
            gunTasks += IsAlive(np) && (FindTask(np.ped, TASK_SIMPLE_USE_GUN) || FindTask(np.ped, TASK_SIMPLE_GANG_DRIVEBY));
        }
        LOGI("Peds: %d announced, %d mirrors (%d spawned, %d animated, %d aiming, %d with a gun task, %d shots)", owned,
             mirrors, spawned, acting, aiming, gunTasks, m_mirrorShots);
        m_mirrorShots = 0;
    }
}

void Peds::OnSessionEnd() {
    // mirrors stay in the world as ordinary random peds
    for (auto& [id, np] : m_byId)
        if (np.owner != Net().MyId() && IsAlive(np)) {
            Cmd(0x02AB).I(np.handle).I(0).I(0).I(0).I(0).I(0).Run();
            Cmd(0x01C2).I(np.handle).Run();  // MARK_CHAR_AS_NO_LONGER_NEEDED (default decision maker, wander task)
        }
    m_byId.clear();
    m_byPtr.clear();
    m_wasReady = false;
}

// The player left: its people stay in this game as its own (deleted, they left its cars driverless in the street, as MTA
// keeps the elements and picks a new syncer).
void Peds::OnPeerLeave(uint8_t id) { DropMirrorsOf(id, true); }

void Peds::DropMirrorsOf(uint8_t id, bool keep) {
    std::vector<uint16_t> mirrors;
    for (auto& [pid, np] : m_byId)
        if (np.owner == id) mirrors.push_back(pid);
    for (uint16_t pid : mirrors) {
        NetPed& np = m_byId[pid];
        bool adopt = keep && IsAlive(np) && !IsPedDead(np.ped);
        if (adopt) Adopt(np);
        Forget(pid, !adopt);
    }
}

void Peds::OnGameStarted() {
    m_byId.clear();
    m_byPtr.clear();
    m_giveAway.clear();
    m_wasReady = false;  // ask the partner for its peds again once the player is ready
}

void Peds::OnGameShutdown() { OnGameStarted(); }

}  // namespace coop
