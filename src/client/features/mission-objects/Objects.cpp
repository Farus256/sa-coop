// Objects of the host's mission (the bottles of "Nines and AKs", boxes, barriers) for the partners. The host's game
// knows what its mission script created (CTheScripts::MissionCleanUp lists them, objects as type 3): it announces
// those, sends their moves and their end, and applies the hits the partner's player lands on the copies. The copies
// are frozen, and nothing in the partner's game damages them: a hit by its player goes to the host as
// CObject::ObjectDamage, and a copy breaks when the host's object did.
#include <cmath>
#include <map>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "entities/vehicle/VehicleState.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

constexpr uintptr_t kMissionCleanUp = 0xA90850;  // CTheScripts::MissionCleanUp: 75 x {uint8 type, int32 handle}
enum : uint8_t { REMOVE_GONE = 0, REMOVE_BROKE = 1 };

struct NetObject {
    uint16_t netId = 0;
    CObject* obj = nullptr;
    int handle = -1;
    uint16_t model = 0;
    CVector pos;
    Quat rot;
    int8_t area = 0;
    bool broken = false;
    bool adopted = false;  // partner: its world had this object already (the host's world brought it)
    // attached to a vehicle in the host's game (a box in a van, a tarp on a train): there it never touches its vehicle,
    // here a solid copy sat inside the vehicle's copy and lifted it (a van floated)
    bool attached = false;
    float scale = 1.0f;  // CObject::m_fScale (SET_OBJECT_SCALE)
    uint64_t sentMs = 0;
};

std::map<uint16_t, NetObject> g_owned;    // host: announced
std::map<uint16_t, NetObject> g_mirrors;  // partner: the copies
std::map<uint16_t, NetObject> g_pending;  // partner: waiting for the model
uint16_t g_nextId = 1;
uint64_t g_scanMs = 0;
bool g_applying = false;  // a damage this module applies itself

bool Alive(const NetObject& no) { return no.obj && IsObjectValid(no.obj) && GetObjectRef(no.obj) == no.handle; }
bool Broken(CObject* o) { return !o->IsVisible() && !(o->Flags() & 1); }  // smashed: no rendering, no collision
Quat RotationOf(CObject* o) { return o->Matrix() ? Quat::FromMatrix(*o->Matrix()) : Quat{}; }

// the copy as the host's object: no collision while that rides attached, its size
void ApplyRide(NetObject& no) {
    if (!Alive(no) || Broken(no.obj)) return;
    Cmd(0x0382).I(no.handle).I(no.attached ? 0 : 1).Run();  // SET_OBJECT_COLLISION
    Field<float>(no.obj, 0x15C) = no.scale;                // CObject::m_fScale (CObject::PreRender scales it)
}

void Place(CObject* o, const CVector& pos, const Quat& rot) {
    CMatrix* m = o->Matrix();
    if (!m) return;
    WorldRemove(o);
    rot.ToMatrix(*m);
    m->pos = pos;
    CallMethod<0x446F90>(o);  // CEntity::UpdateRW
    CallMethod<0x532B00>(o);  // CEntity::UpdateRwFrame
    Call<0x563220>(o);        // CWorld::Add
}

void SendCreate(const NetObject& no, proto::PlayerMask to = proto::kAllPlayers) {
    Writer w(proto::G_OBJ_CREATE);
    w.U16(no.netId);
    w.U16(no.model);
    w.Vec({no.pos.x, no.pos.y, no.pos.z});
    no.rot.Write(w);
    w.I8(no.area);
    w.U8(no.broken);
    w.U8(no.attached);
    w.F32(no.scale);
    Net().SendTo(to, proto::CH_RELIABLE, w);
}

void SendRemove(uint16_t netId, uint8_t kind, uint8_t weapon) {
    Writer w(proto::G_OBJ_REMOVE);
    w.U16(netId);
    w.U8(kind);
    w.U8(weapon);
    Net().Send(proto::CH_RELIABLE, w);
}

NetObject* FindByObject(std::map<uint16_t, NetObject>& m, CObject* o) {
    for (auto& [id, no] : m)
        if (no.obj == o && Alive(no)) return &no;
    return nullptr;
}

// host: announce new mission objects, report moves, breaks and ends
void HostScan() {
    if (HostReplicating()) {
        for (int i = 0; i < 75; ++i) {
            uintptr_t e = kMissionCleanUp + (uintptr_t)i * 8;
            if (Ref<uint8_t>(e) != 3) continue;  // MISSION_CLEANUP_ENTITY_TYPE_OBJECT
            int h = Ref<int32_t>(e + 4);
            CObject* o = GetObject(h);
            if (!o || FindByObject(g_owned, o)) continue;
            NetObject no;
            no.netId = g_nextId++;
            no.obj = o;
            no.handle = h;
            no.model = o->ModelIndex();
            no.pos = o->GetPosition();
            no.rot = RotationOf(o);
            no.area = o->AreaCode();
            no.broken = Broken(o);
            no.attached = o->AttachedTo() != nullptr;
            no.scale = Field<float>(o, 0x15C);
            no.sentMs = NowMs();
            g_owned[no.netId] = no;
            SendCreate(no);
            LOGI("Mission object %04X (model %d) announced", no.netId, no.model);
        }
    }
    for (auto it = g_owned.begin(); it != g_owned.end();) {
        NetObject& no = it->second;
        if (!Alive(no) || no.obj->ModelIndex() != no.model) {
            SendRemove(no.netId, REMOVE_GONE, 0);
            it = g_owned.erase(it);
            continue;
        }
        if (Broken(no.obj)) {
            if (!no.broken) {
                no.broken = true;
                SendRemove(no.netId, REMOVE_BROKE, Field<uint8_t>(no.obj, 0x148));  // CObject::m_nLastWeaponDamage
                LOGI("Mission object %04X broke", no.netId);
            }
        } else {
            no.broken = false;
            CVector pos = no.obj->GetPosition();
            Quat rot = RotationOf(no.obj);
            float turn = std::fabs(rot.x - no.rot.x) + std::fabs(rot.y - no.rot.y) + std::fabs(rot.z - no.rot.z) +
                         std::fabs(rot.w - no.rot.w);
            bool attached = no.obj->AttachedTo() != nullptr;
            float scale = Field<float>(no.obj, 0x15C);
            // and again every 2 s at rest: the state channel can lose the last one, and a pushed box stood half-way there
            bool moved = (pos - no.pos).Length() > 0.02f || turn > 0.01f || attached != no.attached || scale != no.scale;
            if ((moved && NowMs() - no.sentMs >= 100) || NowMs() - no.sentMs >= 2000) {
                no.pos = pos;
                no.rot = rot;
                no.attached = attached;
                no.scale = scale;
                no.sentMs = NowMs();
                Writer w(proto::G_OBJ_SYNC);
                w.U16(no.netId);
                w.Vec({pos.x, pos.y, pos.z});
                rot.Write(w);
                w.U8(attached);
                w.F32(scale);
                Net().Send(proto::CH_STATE, w);
            }
        }
        ++it;
    }
}

// partner: break the copy as the host's object broke (the same damage path shows the same shards)
void BreakCopy(NetObject& no, uint8_t weapon) {
    if (!Alive(no) || Broken(no.obj)) return;
    CVector p = no.obj->GetPosition(), d{0, 0, 1};
    g_applying = true;
    CallMethod<0x5A0D90>(no.obj, 10000.0f, &p, &d, (CEntity*)nullptr, (int)weapon);  // CObject::ObjectDamage
    g_applying = false;
    if (!Broken(no.obj)) {  // nothing to smash on it: just gone
        Cmd(0x0750).I(no.handle).I(0).Run();  // SET_OBJECT_VISIBLE
        Cmd(0x0382).I(no.handle).I(0).Run();  // SET_OBJECT_COLLISION
    }
}

void DeleteCopy(NetObject& no) {
    if (Alive(no) && !no.adopted) Cmd(0x0108).I(no.handle).Run();  // DELETE_OBJECT
}

// the same object in the same place, not a copy yet: the host's world, loaded here, brought it along
CObject* FindTwin(const NetObject& no) {
    CPoolHeader* pool = ObjectPool();
    for (int i = 0; pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* o = (CObject*)(pool->objects + (size_t)i * kObjectPoolElemSize);
        if (o->ModelIndex() != no.model || (o->GetPosition() - no.pos).Length() > 0.25f || FindByObject(g_mirrors, o)) continue;
        return o;
    }
    return nullptr;
}

void ClearCopies() {
    for (auto& [id, no] : g_mirrors) DeleteCopy(no);
    g_mirrors.clear();
    g_pending.clear();
}

void CreatePending() {
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        NetObject no = it->second;
        if (!IsModelLoaded(no.model)) {
            RequestModel(no.model);
            ++it;
            continue;
        }
        if (!HasRoom(ObjectPool(), kKeepObjects, "object")) break;
        it = g_pending.erase(it);
        int h = -1;
        CObject* o = FindTwin(no);
        no.adopted = o != nullptr;
        if (o) h = GetObjectRef(o);
        else Cmd(0x029B).I(no.model).F(no.pos.x).F(no.pos.y).F(no.pos.z).Out(&h).Run();  // CREATE_OBJECT_NO_OFFSET
        if (!o) {
            o = h >= 0 ? GetObject(h) : nullptr;
            if (o) Field<uint8_t>(o, 0x13C) = 6;  // OBJECT_MISSION2: kept out of saves (see Props)
        }
        if (!o) continue;
        Cmd(0x0550).I(h).I(1).Run();  // FREEZE_OBJECT_POSITION
        o->AreaCode() = no.area;
        Place(o, no.pos, no.rot);
        no.obj = o;
        no.handle = h;
        bool broken = no.broken;
        no.broken = false;
        NetObject& copy = g_mirrors[no.netId] = no;
        ApplyRide(copy);
        if (broken) BreakCopy(copy, 0);
        LOGI("Mission object %04X (model %d) %s", no.netId, no.model, no.adopted ? "found here" : "copied");
    }
}

// The partner's player hits a copy: the host's object takes it. Nothing else damages a copy.
using ObjectDamageFn = void(__fastcall*)(CObject*, void*, float, CVector*, CVector*, CEntity*, int);
ObjectDamageFn g_origObjectDamage = nullptr;
// the other game's car or person, as this game's copy of it
bool OtherGames(CEntity* e) {
    if (e->IsVehicle()) {
        NetVehicle* nv = Vehicles::Instance().Find((CVehicle*)e);
        return nv && Vehicles::Instance().IsMirror(*nv);
    }
    if (!e->IsPed()) return false;
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.ped == e) return true;
    NetPed* np = Peds::Instance().Find((CPed*)e);
    return np && np->owner != Net().MyId();
}

void __fastcall HookObjectDamage(CObject* o, void* edx, float damage, CVector* origin, CVector* dir, CEntity* damager,
                                 int weapon) {
    // A map object (a lamp post, a sign) breaks in the game whose car or person really hit it, and WorldObjects sends
    // that over. This game's copy of the other game's car runs a little off the real one's path: it broke posts
    // the real one went past, and the other player saw them fall by themselves.
    // The host's mission objects the same: the other player's game sends its hits (G_OBJ_DAMAGE), and the replayed shots
    // of its copy here counted them twice.
    if (!g_applying && damager && Net().Online() && (Field<uint8_t>(o, 0x13C) == 1 || FindByObject(g_owned, o)) &&
        OtherGames(damager))
        return;
    if (!g_applying && Net().Online() && !g_mirrors.empty()) {
        if (NetObject* no = FindByObject(g_mirrors, o)) {
            CPlayerPed* me = FindPlayerPed(0);
            if (damager && me && (damager == me || (me->InVehicle() && damager == (CEntity*)me->Vehicle()))) {
                Writer w(proto::G_OBJ_DAMAGE);
                w.U16(no->netId);
                w.F32(damage);
                w.U8((uint8_t)weapon);
                Net().SendTo(Net().HostId(), proto::CH_RELIABLE, w);
            }
            return;
        }
    }
    g_origObjectDamage(o, edx, damage, origin, dir, damager, weapon);
}

// The host's mission asks whether its people and objects were photographed ("Badlands", "Photo Opportunity",
// "Architectural Espionage"), and the partner's camera marked only their copies: the partner's game tells the host which.
constexpr uint32_t kPedPhotographed = 1u << 18;  // CPed::bHasBeenPhotographed (the second flags)
constexpr uint32_t kObjectPhotographed = 0x1000;  // CObject::bIsPhotographed
using TakePhotoFn = bool(__fastcall*)(void*, void*, CEntity*, CVector*);
TakePhotoFn g_origTakePhoto = nullptr;
bool __fastcall HookTakePhotograph(void* weapon, void* edx, CEntity* owner, CVector* point) {
    bool taken = g_origTakePhoto(weapon, edx, owner, point);  // false for the other player's camera (Players)
    LOGD("Camera shot by %s: %s (camera mode %d)", owner == (CEntity*)FindPlayerPed(0) ? "us" : "another", taken ? "taken" : "none",
         (int)cam::Mode());
    if (!taken || !Net().Online() || Net().IsHost() || !core::IsPlayerReady()) return taken;
    std::vector<uint16_t> peds, objects;
    CPoolHeader* pool = PedPool();
    for (int i = 0; pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* p = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
        NetPed* np = Peds::Instance().Find(p);
        if (!np || np->owner != Net().HostId() || !(p->PedFlags2() & kPedPhotographed) || peds.size() >= 100) continue;
        p->PedFlags2() &= ~kPedPhotographed;  // told once
        peds.push_back(np->netId);
    }
    for (auto& [id, no] : g_mirrors)
        if (Alive(no) && (Field<uint32_t>(no.obj, 0x140) & kObjectPhotographed) && objects.size() < 100) {
            Field<uint32_t>(no.obj, 0x140) &= ~kObjectPhotographed;
            objects.push_back(id);
        }
    if (peds.empty() && objects.empty()) return taken;
    Writer w(proto::G_PHOTO);
    w.U8((uint8_t)peds.size());
    for (uint16_t id : peds) w.U16(id);
    w.U8((uint8_t)objects.size());
    for (uint16_t id : objects) w.U16(id);
    Net().Send(proto::CH_RELIABLE, w);
    LOGI("Our photo holds %d of the host's people and %d of its objects", (int)peds.size(), (int)objects.size());
    return taken;
}

void HandlePhoto(uint8_t from, Reader& r) {
    if (!Net().IsHost() || !core::IsPlayerReady()) return;
    int peds = 0, objects = 0;
    for (int n = r.U8(); n > 0 && r.Ok(); --n)
        if (CPed* p = Peds::Instance().FindOwned(r.U16()); p && r.Ok()) p->PedFlags2() |= kPedPhotographed, ++peds;
    for (int n = r.U8(); n > 0 && r.Ok(); --n)
        if (auto it = g_owned.find(r.U16()); r.Ok() && it != g_owned.end() && Alive(it->second))
            Field<uint32_t>(it->second.obj, 0x140) |= kObjectPhotographed, ++objects;
    LOGI("Player %d photographed %d of our people and %d of our objects", from, peds, objects);
}

class Objects : public Module {
public:
    const char* Name() const override { return "Objects"; }

    void OnInstallHooks() override {
        mem::HookFn(0x5A0D90, &HookObjectDamage, &g_origObjectDamage, "CObject::ObjectDamage");
        mem::RedirectCallFn(0x74278B, &HookTakePhotograph, &g_origTakePhoto);  // CWeapon::Fire, the camera (Players' too)
        Net().SetHandler(proto::G_PHOTO, &HandlePhoto);
        Net().SetHandler(proto::G_OBJ_CREATE, [](uint8_t from, Reader& r) {
            NetObject no;
            no.netId = r.U16();
            no.model = r.U16();
            Vec3 p = r.Vec();
            no.pos = {p.x, p.y, p.z};
            no.rot.Read(r);
            no.area = r.I8();
            no.broken = r.U8() != 0;
            no.attached = r.U8() != 0;
            no.scale = r.F32();
            if (!std::isfinite(no.scale) || no.scale <= 0.0f || no.scale > 50.0f) no.scale = 1.0f;
            // not in the game yet (a partner waits in the menu): the host announces them again once it is (resync)
            if (!r.Ok() || from != Net().HostId() || Net().IsHost() || !core::IsPlayerReady()) return;
            if (!IsObjectModel(no.model)) {  // a model of a mod this game lacks: the streamer crashes on it
                LOGW("Mission object %04X has an unknown model %d", no.netId, no.model);
                return;
            }
            if (auto it = g_mirrors.find(no.netId); it != g_mirrors.end()) {  // announced again: a fresh copy
                DeleteCopy(it->second);
                g_mirrors.erase(it);
            }
            g_pending[no.netId] = no;
            RequestModel(no.model);
        });
        Net().SetHandler(proto::G_OBJ_SYNC, [](uint8_t from, Reader& r) {
            uint16_t id = r.U16();
            Vec3 p = r.Vec();
            Quat q;
            q.Read(r);
            bool attached = r.U8() != 0;
            float scale = r.F32();
            if (!r.Ok() || from != Net().HostId() || !core::IsPlayerReady()) return;
            if (!std::isfinite(scale) || scale <= 0.0f || scale > 50.0f) scale = 1.0f;
            if (auto it = g_pending.find(id); it != g_pending.end()) {
                it->second.pos = {p.x, p.y, p.z};
                it->second.rot = q;
                it->second.attached = attached;
                it->second.scale = scale;
            } else if (auto m = g_mirrors.find(id); m != g_mirrors.end() && Alive(m->second)) {
                Place(m->second.obj, {p.x, p.y, p.z}, q);
                if (m->second.attached != attached || m->second.scale != scale) {
                    m->second.attached = attached;
                    m->second.scale = scale;
                    ApplyRide(m->second);
                }
            }
        });
        Net().SetHandler(proto::G_OBJ_REMOVE, [](uint8_t from, Reader& r) {
            uint16_t id = r.U16();
            uint8_t kind = r.U8(), weapon = r.U8();
            if (!r.Ok() || from != Net().HostId() || !core::IsPlayerReady()) return;
            if (kind == REMOVE_BROKE) {
                if (auto it = g_pending.find(id); it != g_pending.end()) it->second.broken = true;
                if (auto it = g_mirrors.find(id); it != g_mirrors.end()) BreakCopy(it->second, weapon);
                return;
            }
            g_pending.erase(id);
            if (auto it = g_mirrors.find(id); it != g_mirrors.end()) {
                DeleteCopy(it->second);
                g_mirrors.erase(it);
            }
        });
        Net().SetHandler(proto::G_OBJ_DAMAGE, [](uint8_t from, Reader& r) {
            uint16_t id = r.U16();
            float damage = r.F32();
            uint8_t weapon = r.U8();
            if (!r.Ok() || !Net().IsHost() || !core::IsPlayerReady()) return;
            auto it = g_owned.find(id);
            if (it == g_owned.end() || !Alive(it->second)) return;
            CObject* o = it->second.obj;
            CVector p = o->GetPosition(), d{0, 0, 1};
            // the mission counts its player's hits: the partner's count as his
            CallMethod<0x5A0D90>(o, damage, &p, &d, (CEntity*)FindPlayerPed(0), (int)weapon);  // CObject::ObjectDamage
            LOGI("Mission object %04X hit by player %d (%.0f)", id, from, damage);
        });
    }

    void OnSessionEnd() override {
        ClearCopies();
        g_owned.clear();
    }
    void OnPeerLeave(uint8_t id) override {
        if (id == Net().HostId() || Net().IsHost()) ClearCopies();
    }
    void OnPeerResync(uint8_t id) override {  // that game alone: the others would make their copies again
        if (!Net().IsHost()) {  // the host's game started over: the objects of its old world are gone
            if (id == Net().HostId()) ClearCopies();
            return;
        }
        for (auto& [netId, no] : g_owned)
            if (Alive(no)) SendCreate(no, proto::Bit(id));
    }
    void OnGameStarted() override {  // a new world: the objects of the old one are gone
        g_owned.clear();
        g_mirrors.clear();
        g_pending.clear();
    }
    void OnGameShutdown() override { OnGameStarted(); }  // nothing to touch while the world loads

    void OnPostProcess() override {
        if (!Net().Online() || !core::IsPlayerReady()) return;
        if (Net().IsHost()) {
            if (NowMs() - g_scanMs >= 100) {
                g_scanMs = NowMs();
                HostScan();
            }
        } else if (!g_pending.empty()) {
            CreatePending();
        }
    }

    static Objects& Instance() {
        static Objects m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Objects::Instance()); }
} g_reg;
}  // namespace

}  // namespace coop
