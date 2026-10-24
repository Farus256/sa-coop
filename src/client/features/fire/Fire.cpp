// Fires, explosions, projectiles and water cannons the games share. Fires on the ground from explosions and molotovs start in each
// game by themselves (those are synced, see below), so only these travel: a fire on a player, a ped or a vehicle this game owns (shot with a molotov,
// a flamethrower, a burning car), and the host's mission fires (START_SCRIPT_FIRE). The other game lights the same
// fire on its copy of the entity, or at the spot: such a fire never spreads, never burns the copy (copies are
// fireproof, the proof is lifted only to light it) and goes out when the owner's fire does. A copy this game's player
// puts out (water, an extinguisher) puts the owner's fire out too: the partner helps with the host's firefighting.
#include <algorithm>
#include <cmath>
#include <map>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Challenge.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

constexpr uintptr_t kFireManager = 0xB71F80;  // gFireManager: CFire m_aFires[60], 0x28 bytes each
constexpr int kFires = 60;
constexpr uint32_t kFireProof = 1u << 19;     // CPhysical::physicalFlags.bFireProof
enum : uint8_t { FIRE_START = 1, FIRE_STOP = 2, FIRE_OUT = 3 };  // OUT: a copy was put out, to the owner
enum : uint8_t { ON_SPOT = 0, ON_VEHICLE = 1, ON_PED = 2, ON_PLAYER = 3 };

uintptr_t Fire(int i) { return kFireManager + (uintptr_t)i * 0x28; }
bool Active(uintptr_t f) { return Ref<uint8_t>(f) & 1; }
CEntity* Target(uintptr_t f) { return Ref<CEntity*>(f + 0x10); }

struct Sent {
    uint16_t id;
    CEntity* target;
};
std::map<uintptr_t, Sent> g_sent;         // our fires the other game copied
struct Copy {
    uintptr_t fire;
    uint8_t from;      // whose fire it copies
    uint16_t id;       // its id there
    CEntity* target;   // what it burns (nullptr: a spot)
    CVector at;        // a spot's place
};
std::map<uint32_t, Copy> g_copies;    // fires lit here for the other games, by sender and its id
uint32_t CopyKey(uint8_t from, uint16_t id) { return (uint32_t)from << 16 | id; }
uint16_t g_nextId = 1;
uint64_t g_scanMs = 0;

// The copy's own fire still: the fire manager gives a slot that went out to the next fire (a molotov lights several),
// and a fire of this game taken for the copy was never sent, and the owner's FIRE_STOP put it out.
bool Burns(const Copy& c) {
    if (!Active(c.fire) || Target(c.fire) != c.target) return false;
    return c.target || (Ref<CVector>(c.fire + 0x04) - c.at).Length() < 0.5f;
}

bool IsCopy(uintptr_t f) {
    for (auto& [id, c] : g_copies)
        if (c.fire == f && Burns(c)) return true;
    return false;
}

// this game's player could have put it out: close, with water or an extinguisher (a fire truck's cannon, weapon 42)
bool PlayerFighting(const CVector& at) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || (me->GetPosition() - at).Length() > 30.0f) return false;
    if (me->InVehicle()) return me->Vehicle() && me->Vehicle()->ModelIndex() == 407;  // FIRETRUK
    return me->ActiveWeapon().type == 42;
}

// what the other game knows this burning entity as; false when it does not know it
bool Describe(CEntity* e, uint8_t& kind, uint16_t& id) {
    if (!e) return false;
    if (e->IsPed()) {
        if (e == (CEntity*)FindPlayerPed(0)) return kind = ON_PLAYER, id = Net().MyId(), true;
        NetPed* np = Peds::Instance().Find((CPed*)e);
        return np && np->owner == Net().MyId() && (kind = ON_PED, id = np->netId, true);
    }
    if (e->Type() == ENTITY_TYPE_VEHICLE) {
        NetVehicle* nv = Vehicles::Instance().Find((CVehicle*)e);
        return nv && !Vehicles::Instance().IsMirror(*nv) && (kind = ON_VEHICLE, id = nv->netId, true);
    }
    return false;
}

// the copy of what burns in the other game
CEntity* Resolve(uint8_t kind, uint16_t id) {
    if (kind == ON_VEHICLE) {
        NetVehicle* nv = Vehicles::Instance().Find(id);
        return nv && nv->veh && IsVehicleValid(nv->veh) ? (CEntity*)nv->veh : nullptr;
    }
    if (kind == ON_PED) return (CEntity*)GetPed(Peds::Instance().HandleOf(id));
    if (kind == ON_PLAYER) {
        RemotePlayer* rp = Players::Instance().Get((uint8_t)id);
        return rp ? (CEntity*)rp->ped : nullptr;
    }
    return nullptr;
}

void SendStop(uint16_t id) {
    Writer w(proto::G_FIRE);
    w.U8(FIRE_STOP);
    w.U16(id);
    Net().Send(proto::CH_RELIABLE, w);
}

void Scan() {
    for (int i = 0; i < kFires; ++i) {
        uintptr_t f = Fire(i);
        auto sent = g_sent.find(f);
        if (!Active(f) || IsCopy(f)) {
            if (sent != g_sent.end()) {
                SendStop(sent->second.id);
                g_sent.erase(sent);
            }
            continue;
        }
        CEntity* target = Target(f);
        if (sent != g_sent.end()) {
            if (sent->second.target == target) continue;
            SendStop(sent->second.id);  // the slot holds another fire now
            g_sent.erase(sent);
        }
        uint8_t kind = ON_SPOT;
        uint16_t entityId = 0;
        bool script = Ref<uint8_t>(f) & 2;  // bCreatedByScript
        if (target ? !Describe(target, kind, entityId) : !(script && Net().IsHost() && HostReplicating())) continue;
        uint16_t id = g_nextId++;
        g_sent[f] = {id, target};
        CVector pos = Ref<CVector>(f + 0x04);
        Writer w(proto::G_FIRE);
        w.U8(FIRE_START);
        w.U16(id);
        w.U8(kind);
        w.U16(entityId);
        w.Vec({pos.x, pos.y, pos.z});
        w.F32(Ref<float>(f + 0x1C));  // m_fStrength
        Net().Send(proto::CH_RELIABLE, w);
        LOGD("Fire %04X sent (on %d %04X)", id, kind, entityId);
    }
}

void Light(uint8_t from, uint16_t id, uint8_t kind, uint16_t entityId, const CVector& pos, float strength) {
    uint32_t key = CopyKey(from, id);
    uintptr_t fire = 0;
    if (kind == ON_SPOT) {
        // CFireManager::StartFire(CVector, size, unused, creator, lifetime, generations, unused): no spreading
        fire = CallMethod<0x539F00, uintptr_t>((void*)kFireManager, pos.x, pos.y, pos.z, strength, (uint8_t)1,
                                                (CEntity*)nullptr, (uint32_t)600000, (int8_t)0, (uint8_t)1);
        if (fire && strength > 1.0f) {  // it lights every fire at strength 1 (CFire::Start): a mission's big fire too
            Ref<float>(fire + 0x1C) = strength;
            CVector at = Ref<CVector>(fire + 0x04);
            CallMethod<0x539360>((void*)fire, &at, (void*)nullptr);  // CFire::CreateFxSysForStrength
        }
    } else {
        CEntity* e = Resolve(kind, entityId);
        if (!e) return;
        auto* p = (CPhysical*)e;
        uint32_t flags = p->PhysicalFlags();
        p->PhysicalFlags() = flags & ~kFireProof;
        // CFireManager::StartFire(target, creator, size, unused, lifetime, generations)
        CallMethod<0x53A050, void*>((void*)kFireManager, e, (CEntity*)nullptr, 0.8f, (uint8_t)1, (uint32_t)600000, (int8_t)0);
        p->PhysicalFlags() = flags;
        fire = Field<uintptr_t>(e, e->IsPed() ? 0x730 : 0x490);  // CPed::m_pFire, CVehicle::m_pFire
    }
    if (!fire) return;
    g_copies[key] = {fire, from, id, Target(fire), Ref<CVector>(fire + 0x04)};
    LOGD("Fire %04X of player %d lit (on %d %04X)", id, from, kind, entityId);
}

void PutOut(uint32_t key) {
    auto it = g_copies.find(key);
    if (it == g_copies.end()) return;
    if (Burns(it->second)) CallMethod<0x5393F0>((void*)it->second.fire);  // CFire::Extinguish
    g_copies.erase(it);
}

// a player's fires are gone with its game (it left or started over): theirs here burned on for ten minutes
void PutOutCopiesOf(uint8_t from) {
    for (auto it = g_copies.begin(); it != g_copies.end();) {
        if (it->second.from != from) {
            ++it;
            continue;
        }
        if (Burns(it->second)) CallMethod<0x5393F0>((void*)it->second.fire);  // CFire::Extinguish
        it = g_copies.erase(it);
    }
}

// the owner's side of FIRE_OUT: its fire goes out as the copy did
void PutOutOwn(uint16_t id) {
    for (auto it = g_sent.begin(); it != g_sent.end(); ++it)
        if (it->second.id == id) {
            if (Active(it->first)) CallMethod<0x5393F0>((void*)it->first);  // CFire::Extinguish
            g_sent.erase(it);
            LOGI("Fire %04X put out by the other player", id);
            return;
        }
}

// ---------------------------------------------------------------- explosions
// A blast (a grenade, a rocket, a Rhino's shell, a bomb, a mission's explosion) goes off in the game of whoever set it
// off, and the other game sets off the same one in the name of its copy of the creator: its damage there falls on
// what that game owns (Damage.cpp). A blast here of the other side's copy (a rocket or a grenade its copy of the other
// player fired) is left out, the real one comes. Cars, boats, planes and objects blow up in each game by their own
// sync, and a molotov's burst lights the fires each game has.
bool IsBlast(int type) {  // eExplosionType: GRENADE, ROCKET, WEAK_ROCKET, MINE, TANK_FIRE, SMALL
    return type == 0 || type == 2 || type == 3 || type == 8 || type == 10 || type == 11;
}

// the other player, its car, or a ped or a car of the other game (a copy here)
bool OtherSide(CEntity* e) {
    if (!e) return false;
    if (e->IsPed()) {
        NetPed* np = Peds::Instance().Find((CPed*)e);
        return Players::Instance().FindByPed((CPed*)e) || (np && np->owner != Net().MyId());
    }
    NetVehicle* nv = e->Type() == ENTITY_TYPE_VEHICLE ? Vehicles::Instance().Find((CVehicle*)e) : nullptr;
    return nv && Vehicles::Instance().IsMirror(*nv);
}

using AddExplosionFn = void(__cdecl*)(CEntity*, CEntity*, int, CVector, uint32_t, uint8_t, float, uint8_t);
AddExplosionFn g_origAddExplosion = nullptr;
bool g_settingOff = false;  // a blast of the other game

// CExplosion::AddExplosion(victim, creator, type, position, lifetime, sound, camera shake, visible)
void __cdecl HookAddExplosion(CEntity* victim, CEntity* creator, int type, CVector pos, uint32_t lifetime, uint8_t sound,
                              float shake, uint8_t visible) {
    if (Net().Online() && core::IsPlayerReady() && IsBlast(type) && !g_settingOff) {
        if (OtherSide(creator)) return;
        uint8_t kind = ON_SPOT;
        uint16_t id = 0;
        if (!Describe(creator, kind, id)) kind = ON_SPOT, id = 0;  // nobody's, a SAM site's: on the spot
        Writer w(proto::G_EXPLOSION);
        w.U8(kind);
        w.U16(id);
        w.U8((uint8_t)type);
        w.Vec({pos.x, pos.y, pos.z});
        w.U32(lifetime);
        w.U8(sound);
        w.F32(shake);
        w.U8(visible);
        Net().Send(proto::CH_RELIABLE, w);
    }
    g_origAddExplosion(victim, creator, type, pos, lifetime, sound, shake, visible);
}

void DropCharges(CEntity* creator, const CVector& at, float within);

void HandleExplosion(uint8_t from, Reader& r) {
    uint8_t kind = r.U8();
    uint16_t id = r.U16();
    int type = r.U8();
    Vec3 p = r.Vec();
    uint32_t lifetime = r.U32();
    uint8_t sound = r.U8();
    float shake = r.F32();
    uint8_t visible = r.U8();
    if (!r.Ok() || !core::IsPlayerReady() || !IsBlast(type)) return;
    CEntity* creator = Resolve(kind, id);
    if (kind != ON_SPOT && !creator) return;  // its creator is not here: far away, in another interior
    g_settingOff = true;
    g_origAddExplosion(nullptr, creator, type, CVector{p.x, p.y, p.z}, lifetime, sound, shake, visible);
    g_settingOff = false;
    // the real blast of a satchel charge: its copy here goes (the replayed detonator press may have been lost)
    // shortcut: a grenade of the same player within 3 m takes a charge's copy too, a charge id in G_EXPLOSION would end it
    if (type == 0 && creator) DropCharges(creator, CVector{p.x, p.y, p.z}, 3.0f);
    LOGD("Explosion of player %d set off (type %d, by %d %04X)", from, type, kind, id);
}

// ---------------------------------------------------------------- projectiles
// A grenade, a molotov, tear gas, a rocket, a bomb, a satchel charge or a flare flies in the game that threw or fired
// it, and the other game throws the same one from the same spot at the same speed in the name of its copy of the
// thrower, to be seen: it goes off there without harm (a blast of the other side is left out, above) and the real blast
// comes. A projectile here of the other side's copy (its copy of the other player throwing) is left out the same way.
// A satchel charge's copy lies where it landed until its thrower's copy sets it off (blast left out) or the real blast
// comes (the other player saw the charge in the thrower's hand, never where it flew or lay).
constexpr uintptr_t kProjectileInfos = 0xC891A8;  // gaProjectileInfo[32], 0x24 bytes: weapon +0, creator +4, fuse +0xC, m_bActive +0x10
constexpr uintptr_t kProjectiles = 0xC89110;      // CProjectileInfo::ms_apProjectile[32]
constexpr int WEAPON_SATCHEL = 39;
bool Thrown(int weapon) { return (weapon >= 16 && weapon <= 21) || weapon == WEAPON_SATCHEL || weapon == 58; }  // GRENADE..FREEFALL_BOMB, SATCHEL, FLARE

// satchel charges of `creator` (nullptr: nobody's, the copy of their thrower was deleted) within `within` of `at`, gone
// without a blast
void DropCharges(CEntity* creator, const CVector& at, float within) {
    for (int i = 0; i < 32; ++i) {
        uintptr_t info = kProjectileInfos + (uintptr_t)i * 0x24;
        auto* obj = Ref<CPhysical*>(kProjectiles + (uintptr_t)i * 4);
        if (obj && Ref<int32_t>(info) == WEAPON_SATCHEL && Ref<uint8_t>(info + 0x10) && Ref<CEntity*>(info + 4) == creator &&
            (obj->GetPosition() - at).Length() <= within)
            Call<0x739A40, bool>(obj);  // CProjectileInfo::RemoveIfThisIsAProjectile
    }
}

// CWorld::UseDetonator(user) ends in CProjectileInfo::RemoveDetonatorProjectiles, which sets off every satchel charge
// there is: the other player's detonator, replayed on its copy, blew up this player's own charges. Each sets off its
// own (the copy its copies here, their blasts left out).
using UseDetonatorFn = void(__cdecl*)(CEntity*);
UseDetonatorFn g_origUseDetonator = nullptr;
void __cdecl HookUseDetonator(CEntity* user) {
    uint32_t others = 0;
    for (int i = 0; i < 32; ++i) {
        uintptr_t info = kProjectileInfos + (uintptr_t)i * 0x24;
        if (Ref<int32_t>(info) == WEAPON_SATCHEL && Ref<uint8_t>(info + 0x10) && Ref<CEntity*>(info + 4) != user) {
            others |= 1u << i;
            Ref<uint8_t>(info + 0x10) = 0;  // m_bActive: passed over this once
        }
    }
    g_origUseDetonator(user);
    for (int i = 0; i < 32; ++i)
        if (others & (1u << i)) Ref<uint8_t>(kProjectileInfos + (uintptr_t)i * 0x24 + 0x10) = 1;
}

uint32_t ActiveProjectiles() {
    uint32_t mask = 0;
    for (int i = 0; i < 32; ++i)
        if (Ref<uint8_t>(kProjectileInfos + (uintptr_t)i * 0x24 + 0x10)) mask |= 1u << i;
    return mask;
}
CPhysical* NewProjectile(uint32_t before) {
    uint32_t added = ActiveProjectiles() & ~before;
    for (int i = 0; i < 32; ++i)
        if (added & (1u << i)) return Ref<CPhysical*>(kProjectiles + (uintptr_t)i * 4);
    return nullptr;
}

// a target both games know (the other player, a copy), unlike Describe: a heat-seeking rocket's
bool DescribeAny(CEntity* e, uint8_t& kind, uint16_t& id) {
    if (!e || Describe(e, kind, id)) return e != nullptr;
    if (e->IsPed()) {
        if (RemotePlayer* rp = Players::Instance().FindByPed((CPed*)e)) return kind = ON_PLAYER, id = rp->id, true;
        NetPed* np = Peds::Instance().Find((CPed*)e);
        return np && (kind = ON_PED, id = np->netId, true);
    }
    NetVehicle* nv = e->Type() == ENTITY_TYPE_VEHICLE ? Vehicles::Instance().Find((CVehicle*)e) : nullptr;
    return nv && (kind = ON_VEHICLE, id = nv->netId, true);
}

using AddProjectileFn = bool(__cdecl*)(CEntity*, int, CVector, float, const CVector*, CEntity*);
AddProjectileFn g_origAddProjectile = nullptr;
bool g_throwing = false;  // a projectile of the other game

// CProjectileInfo::AddProjectile(creator, weapon, position, force, direction, target)
bool __cdecl HookAddProjectile(CEntity* creator, int weapon, CVector pos, float force, const CVector* dir, CEntity* target) {
    bool shared = Net().Online() && core::IsPlayerReady() && !g_throwing;
    if (shared && OtherSide(creator)) return false;
    uint32_t before = ActiveProjectiles();
    bool ok = g_origAddProjectile(creator, weapon, pos, force, dir, target);
    CVector throwFrom, throwSpeed;
    if (ok && game::challenge::NextThrow(throwFrom, throwSpeed))  // a battle's grenade thrown by hand: its arc
        if (CPhysical* made = NewProjectile(before)) {
            made->SetPositionRaw(throwFrom);
            made->MoveSpeed() = throwSpeed;
        }
    CPhysical* obj = ok && shared && Thrown(weapon) ? NewProjectile(before) : nullptr;
    if (!obj) return ok;
    uint8_t kind = ON_SPOT, targetKind = ON_SPOT;
    uint16_t id = 0, targetId = 0;
    if (!Describe(creator, kind, id)) kind = ON_SPOT, id = 0;
    if (!DescribeAny(target, targetKind, targetId)) targetKind = ON_SPOT, targetId = 0;
    CVector at = obj->GetPosition(), speed = obj->MoveSpeed();
    Writer w(proto::G_PROJECTILE);
    w.U8(kind);
    w.U16(id);
    w.U8((uint8_t)weapon);
    w.F32(force);
    w.Vec({at.x, at.y, at.z});
    w.Vec({speed.x, speed.y, speed.z});
    w.U8(targetKind);
    w.U16(targetId);
    Net().Send(proto::CH_RELIABLE, w);
    return ok;
}

void HandleProjectile(uint8_t from, Reader& r) {
    uint8_t kind = r.U8();
    uint16_t id = r.U16();
    int weapon = r.U8();
    float force = r.F32();
    Vec3 p = r.Vec(), v = r.Vec();
    uint8_t targetKind = r.U8();
    uint16_t targetId = r.U16();
    if (!r.Ok() || !core::IsPlayerReady() || !Thrown(weapon)) return;
    CEntity* creator = Resolve(kind, id);
    if (kind == ON_SPOT) {  // a SAM site's: in the sender's name, so that it goes off here without harm
        RemotePlayer* rp = Players::Instance().Get(from);
        creator = rp ? (CEntity*)rp->ped : nullptr;
    }
    if (!creator) return;  // far away, in another interior
    CEntity* target = targetKind == ON_PLAYER && targetId == Net().MyId() ? (CEntity*)FindPlayerPed(0)
                      : targetKind == ON_SPOT                             ? nullptr
                                                                          : Resolve(targetKind, targetId);
    CVector at{p.x, p.y, p.z}, speed{v.x, v.y, v.z}, dir = speed;
    float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len > 0.001f) dir = {dir.x / len, dir.y / len, dir.z / len};
    uint32_t before = ActiveProjectiles();
    g_throwing = true;
    bool ok = g_origAddProjectile(creator, weapon, at, force, &dir, target);
    g_throwing = false;
    CPhysical* obj = ok ? NewProjectile(before) : nullptr;
    if (!obj) return;
    obj->SetPositionRaw(at);  // where and how fast it left the other game, whatever the copy's aim here
    obj->MoveSpeed() = speed;
    // its thrower's detonator is looked at without a null check once the fuse time is up (CProjectileInfo::Update,
    // 0x738F3D), and the copy of the thrower can be deleted before the charge: a copy never looks
    if (weapon == WEAPON_SATCHEL)
        for (int i = 0; i < 32; ++i)
            if (Ref<CPhysical*>(kProjectiles + (uintptr_t)i * 4) == obj) Ref<uint32_t>(kProjectileInfos + (uintptr_t)i * 0x24 + 0x0C) = 0;
    LOGD("Projectile of player %d thrown (weapon %d, by %d %04X)", from, weapon, kind, id);
}

// ---------------------------------------------------------------- water cannons
// A fire truck's or a SWAT van's water jet (CWaterCannons::UpdateOne each frame it shoots) shoots from the other game's
// copy of the vehicle too, and puts out and knocks down there what it reaches.
struct Jet {
    CVector offset, speed;  // where it leaves the vehicle, how fast
    uint64_t ms;
};
std::map<uint16_t, Jet> g_jets;          // the other game's, by vehicle
std::map<uint16_t, uint64_t> g_jetSent;  // ours: when each was sent

using WaterCannonFn = void(__cdecl*)(uint32_t, CVector*, CVector*);
WaterCannonFn g_origWaterCannon = nullptr;
void __cdecl HookWaterCannon(uint32_t vehicle, CVector* start, CVector* speed) {
    g_origWaterCannon(vehicle, start, speed);
    if (!Net().Online() || !core::IsPlayerReady()) return;
    NetVehicle* nv = Vehicles::Instance().Find((CVehicle*)(uintptr_t)vehicle);
    if (!nv || Vehicles::Instance().IsMirror(*nv) || !IsVehicleValid(nv->veh) || NowMs() - g_jetSent[nv->netId] < 66) return;
    g_jetSent[nv->netId] = NowMs();
    CVector at = *start - nv->veh->GetPosition();
    Writer w(proto::G_WATER_CANNON);
    w.U16(nv->netId);
    w.Vec({at.x, at.y, at.z});
    w.Vec({speed->x, speed->y, speed->z});
    Net().SendTo(PlayersNear(nv->veh->GetPosition(), nv->veh->AreaCode(), 250.0f, false), proto::CH_STATE, w);
}

// The police helicopter's gun (CHeli::ProcessControl, 0x6C7773: FireOneInstantHitRound from under its nose towards the
// wanted player) fired in its own game only: the other player saw it hover, and its target hit by nothing (issue #22).
// Each round goes over (G_HELI_SHOT) and is fired again from the copy, tracer, flash and sound. The round names no
// shooter, so here it hurts only what this game owns (copies of the other game's people and cars let such hits by):
// every bullet hits once, in the game of what it hits.
using RoundFn = void(__cdecl*)(const CVector*, const CVector*, int);
RoundFn g_origHeliRound = nullptr;

CVehicle* HeliAt(const CVector& gun) {  // the gun is 3.6 m from its helicopter's centre
    CPoolHeader* pool = VehiclePool();
    CVehicle* best = nullptr;
    float bestD = 6.0f;
    for (int i = 0; pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* v = (CVehicle*)(pool->objects + (size_t)i * kVehiclePoolElemSize);
        if (v->VehicleSubClass() != VEHICLE_TYPE_HELI) continue;
        if (float d = (v->GetPosition() - gun).Length(); d < bestD) bestD = d, best = v;
    }
    return best;
}

void __cdecl HookHeliRound(const CVector* start, const CVector* end, int damage) {
    g_origHeliRound(start, end, damage);
    if (!Net().Online() || !core::IsPlayerReady()) return;
    CVehicle* heli = HeliAt(*start);
    NetVehicle* nv = heli ? Vehicles::Instance().Find(heli) : nullptr;
    if (!nv || Vehicles::Instance().IsMirror(*nv)) return;
    CVector gun = *start - heli->GetPosition();
    Writer w(proto::G_HELI_SHOT);
    w.U16(nv->netId);
    w.Vec({gun.x, gun.y, gun.z});
    w.Vec({end->x, end->y, end->z});
    w.U8((uint8_t)std::clamp(damage, 0, 255));
    Net().Send(proto::CH_RELIABLE, w);
    static uint64_t saidMs = 0;
    if (NowMs() - saidMs > 5000) saidMs = NowMs(), LOGD("Helicopter %04X fires: sent", nv->netId);
}

void HandleHeliShot(uint8_t, Reader& r) {
    uint16_t id = r.U16();
    Vec3 g = r.Vec(), e = r.Vec();
    int damage = r.U8();
    NetVehicle* nv = r.Ok() && core::IsPlayerReady() ? Vehicles::Instance().Find(id) : nullptr;
    if (!nv || !Vehicles::Instance().IsMirror(*nv) || !IsVehicleValid(nv->veh) ||
        nv->veh->VehicleSubClass() != VEHICLE_TYPE_HELI)
        return;
    CVector gun{g.x, g.y, g.z}, start = nv->veh->GetPosition() + gun, end{e.x, e.y, e.z};
    if (!(gun.Length() < 10.0f) || !((end - start).Length() < 300.0f)) return;  // NaN fails these too
    Call<0x73AF00>(&start, &end, damage);                       // FireOneInstantHitRound
    CallMethod<0x506F40>((void*)0xB6BC90, 0x91, 0x1F, nv->veh);  // CAudioEngine::ReportWeaponEvent: its gun's sound
    static uint64_t saidMs = 0;
    if (NowMs() - saidMs > 5000) saidMs = NowMs(), LOGD("Helicopter %04X of the other game fires here", id);
}

void ShootJets() {
    for (auto it = g_jets.begin(); it != g_jets.end();) {
        NetVehicle* nv = Vehicles::Instance().Find(it->first);
        if (NowMs() - it->second.ms > 200 || !nv || !IsVehicleValid(nv->veh)) {  // it stopped shooting
            it = g_jets.erase(it);
            continue;
        }
        CVector start = nv->veh->GetPosition() + it->second.offset, speed = it->second.speed;
        g_origWaterCannon((uint32_t)(uintptr_t)nv->veh, &start, &speed);
        ++it;
    }
}

class Fires : public Module {
public:
    const char* Name() const override { return "Fires"; }

    void OnInstallHooks() override {
        mem::HookFn(0x736A50, &HookAddExplosion, &g_origAddExplosion, "CExplosion::AddExplosion");
        Net().SetHandler(proto::G_EXPLOSION, &HandleExplosion);
        mem::HookFn(0x737C80, &HookAddProjectile, &g_origAddProjectile, "CProjectileInfo::AddProjectile");
        mem::HookFn(0x5660B0, &HookUseDetonator, &g_origUseDetonator, "CWorld::UseDetonator");
        Net().SetHandler(proto::G_PROJECTILE, &HandleProjectile);
        mem::HookFn(0x728CB0, &HookWaterCannon, &g_origWaterCannon, "CWaterCannons::UpdateOne");
        mem::RedirectCallFn(0x6C7773, &HookHeliRound, &g_origHeliRound);  // CHeli::ProcessControl -> FireOneInstantHitRound
        Net().SetHandler(proto::G_HELI_SHOT, &HandleHeliShot);
        Net().SetHandler(proto::G_WATER_CANNON, [](uint8_t, Reader& r) {
            uint16_t id = r.U16();
            Vec3 at = r.Vec(), v = r.Vec();
            if (!r.Ok() || !core::IsPlayerReady()) return;
            if (!g_jets.count(id)) LOGD("Water cannon of vehicle %04X shoots here", id);
            g_jets[id] = {{at.x, at.y, at.z}, {v.x, v.y, v.z}, NowMs()};
        });
        Net().SetHandler(proto::G_FIRE, [](uint8_t from, Reader& r) {
            uint8_t op = r.U8();
            uint16_t id = r.U16();
            if (!r.Ok() || !core::IsPlayerReady()) return;
            if (op == FIRE_STOP) return PutOut(CopyKey(from, id));
            if (op == FIRE_OUT) return PutOutOwn(id);
            uint8_t kind = r.U8();
            uint16_t entityId = r.U16();
            Vec3 p = r.Vec();
            float strength = r.F32();
            if (!r.Ok()) return;
            PutOut(CopyKey(from, id));
            Light(from, id, kind, entityId, {p.x, p.y, p.z}, strength);
        });
    }
    void OnSessionEnd() override { Forget(); }
    void OnGameStarted() override { Forget(); }
    void OnPreProcess() override {
        if (Net().Online() && core::IsPlayerReady()) ShootJets();
    }
    void OnPeerResync(uint8_t id) override {
        g_sent.clear();  // sent again by the next scan
        PutOutCopiesOf(id);
    }
    void OnPeerLeave(uint8_t id) override { PutOutCopiesOf(id); }

    void OnPostProcess() override {
        if (!Net().Online() || !core::IsPlayerReady() || NowMs() - g_scanMs < 200) return;
        g_scanMs = NowMs();
        DropCharges(nullptr, CVector{}, 1e9f);  // the copy of their thrower is gone: a projectile slot (32 in all) held for good
        // copies whose fire went out here by itself (water, the entity died) stay out; put out by this player, the
        // owner's goes out too
        for (auto it = g_copies.begin(); it != g_copies.end();) {
            if (Burns(it->second)) {
                ++it;
                continue;
            }
            if (PlayerFighting(Ref<CVector>(it->second.fire + 0x04))) {
                Writer w(proto::G_FIRE);
                w.U8(FIRE_OUT);
                w.U16(it->second.id);
                Net().SendTo(it->second.from, proto::CH_RELIABLE, w);  // its owner's id: for that game alone
                LOGI("Fire %04X of player %d put out here", it->second.id, it->second.from);
            }
            it = g_copies.erase(it);
        }
        Scan();
    }

    static Fires& Instance() {
        static Fires m;
        return m;
    }

private:
    static void Forget() {
        g_sent.clear();
        g_copies.clear();
        g_jets.clear();
        g_jetSent.clear();
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Fires::Instance()); }
} g_reg;
}  // namespace

}  // namespace coop
