// Tanks fight like tanks.
//
// Shells. A Rhino's gun no longer lands its blast at once wherever the barrel points within 60 m (CAutomobile::
// TankControl): it fires a shell that flies (250 m/s, dropping a little, up to 750 m) and bursts where it hits, seen as
// a glowing streak. The gun takes 4 s to load (6 s for the game's crews). A tank shrugs off bullets and blasts as before
// (it is proof against both), but a shell goes through: one in the front armour takes ~18% of its health, in a side
// ~30%, in the rear ~45%; a near miss shakes it a little. Under a quarter it burns and blows up as cars do.
// The game that fires flies the real shell: its blast goes to the others as its explosion (Fire), the others fly a copy
// for the eye (G_TANK_SHOT), and a hit on a tank of another game goes to that game (G_TANK_HIT), whose tank it is.
//
// Crews. A tank driven by the game's people (not a player) aims its turret and fires at players: the army's and the
// police's at a player wanted with four stars or more, a tank battle's at every player in it. The turret turns at a
// tank's pace, leads a moving target and allows for the drop; it fires when the gun is laid and the target in sight.
//
// The tank battle (/tanks, the host): every player within 400 m of the host gets a Rhino (made by its own game, by the
// road) and three waves of enemy tanks come from 250-350 m away (as many as the players, and one more each wave); they
// drive in to 140 m and fight from there. Soldiers with rocket launchers come on foot with each wave (2, 3, 4): a rocket
// takes 30% of a tank's armour. The next wave comes only when every tank and soldier of this one is down (the host
// counts its own, the others are told). All waves beaten: everyone in the battle gets paid. Everyone in it dead, or
// 15 minutes: lost. /tanks stop ends it. No wanted stars in it, and what a player took up there it does not keep
// (game::challenge); the soldiers drop nothing.
// While the battle is on, the games of the players in it make no passers-by and no traffic and clear away the ones
// they have (EmptyStreets), every enemy tank is marked on the screen (a red mark over it, an arrow at the edge when it
// is aside), and the enemies always know where the players are.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "entities/ped/Peds.h"
#include "entities/radar-icons/RadarIcons.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Challenge.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/marks/Marks.h"
#include "shared/ui/renderer/Renderer.h"
#include "shared/ui/widgets/Widgets.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

constexpr int MODEL_RHINO = 432, MODEL_ARMY = 287;
constexpr int kTurretYaw = 0x94C, kTurretPitch = 0x950;  // CAutomobile::m_fDoomVerticalRotation, m_fDoomHorizontalRotation
constexpr int kCarNodes = 0x648, CAR_MISC_C = 22;         // CAutomobile::m_aCarNodes: the gun's end
constexpr uintptr_t kIgnoreEntity = 0xB7CD68;             // CWorld::pIgnoreEntity
constexpr int EXPLOSION_TANK_FIRE = 10;
constexpr int PED_TYPE_COP = 6, PED_TYPE_MISSION1 = 24;
constexpr uint8_t STATUS_WRECKED = 5;

constexpr float kShellSpeed = 250.0f, kGravity = 3.4f, kShellLife = 3.0f;
constexpr uint64_t kReloadPlayerMs = 4000, kReloadCrewMs = 6000;
constexpr float kCrewRange = 350.0f;
std::mt19937 g_rng{(uint32_t)GetTickCount()};
float Rand(float a, float b) { return a + (b - a) * (float)(g_rng() % 10001) / 10000.0f; }

CVector Rotate(CVehicle* v, const CVector& l) {  // vehicle space -> world (direction)
    const CMatrix* m = v->Matrix();
    return m ? m->right * l.x + m->forward * l.y + m->up * l.z : l;
}
CVector Unrotate(CVehicle* v, const CVector& w) {  // world -> vehicle space (direction)
    const CMatrix* m = v->Matrix();
    return m ? CVector{w.Dot(m->right), w.Dot(m->forward), w.Dot(m->up)} : w;
}
float Wrap(float a) {
    // in one step: subtracting 2pi in a loop never ends for a large angle (1e9, the "not set yet" heading of a held
    // cutscene partner, is a multiple of the float step there and froze the game in the cutscene)
    if (!std::isfinite(a)) return 0.0f;
    return std::remainder(a, 6.2831853f);
}

// where the barrel points (TankControl's own way: x = sin(-yaw), y = cos(yaw), z = sin(pitch), in the tank's space)
CVector BarrelDir(CVehicle* v) {
    float yaw = Field<float>(v, kTurretYaw), pitch = Field<float>(v, kTurretPitch);
    return Rotate(v, CVector{std::sin(-yaw), std::cos(yaw), std::sin(pitch)}).Normalized();
}
// the gun's pitch that points it along l (tank space, unit): BarrelDir's z is sin(pitch) over a unit level part,
// so tan(elevation) = sin(pitch)
float PitchFor(const CVector& l) {
    float level = std::sqrt(l.x * l.x + l.y * l.y);
    return std::asin(std::clamp(l.z / std::max(level, 0.01f), -1.0f, 1.0f));
}
CVector Muzzle(CVehicle* v, const CVector& dir) {
    if (void* node = Field<void*>(v, kCarNodes + CAR_MISC_C * 4)) {
        if (auto* ltm = Call<0x7F0990, uint8_t*>(node)) return Ref<CVector>((uintptr_t)ltm + 0x30);  // RwFrameGetLTM: its place
    }
    return v->GetPosition() + Rotate(v, CVector{0, 0, 1.6f}) + dir * 4.0f;
}

// ---------------------------------------------------------------- shells
struct Shell {
    CVector pos, vel;
    float age = 0;
    CVehicle* from = nullptr;  // the tank (left out of its own line)
    CEntity* by = nullptr;     // whose blast it is (the crew or the player)
    bool real = false;         // ours: it bursts and hurts; a copy of another game's is only seen
};
std::vector<Shell> g_shells;

struct ColPoint {
    CVector point;
    uint8_t rest[0x2C - 12];
};

void Hurt(CVehicle* tank, float damage, const CVector& from);

// a player's tank hurt by another player's shell only with the host's friendly fire on (as the players themselves)
bool PlayerTank(CVehicle* v) {
    CPed* seats[9] = {v->Driver()};
    for (int i = 0; i < 8; ++i) seats[i + 1] = v->Passengers()[i];
    for (CPed* p : seats)
        if (p && (p == FindPlayerPed(0) || Players::Instance().FindByPed(p))) return true;
    return false;
}
CEntity* g_hitBy = nullptr;  // whose shell the hit being sent is

void SendHit(CVehicle* tank, float damage, const CVector& from) {
    if (g_hitBy && g_hitBy == FindPlayerPed(0) && PlayerTank(tank) && !Net().FriendlyFire() && Net().Online()) return;
    NetVehicle* nv = Vehicles::Instance().Find(tank);
    if (!nv || !Vehicles::Instance().IsMirror(*nv)) return Hurt(tank, damage, from);  // ours
    Writer w(proto::G_TANK_HIT);
    w.U16(nv->netId);
    w.F32(damage);
    w.Vec({from.x, from.y, from.z});
    Net().SendTo(nv->owner, proto::CH_RELIABLE, w);
}

// a shell's damage to a tank by where it struck: the front armour is the thickest
float ShellDamage(CVehicle* tank, const CVector& travel) {
    CVector l = Unrotate(tank, travel.Normalized());
    float base = l.y < -0.6f ? 180.0f : l.y > 0.6f ? 450.0f : 300.0f;  // coming at its front / its rear / a side
    return base * Rand(0.85f, 1.15f);
}

void Burst(Shell& s, const CVector& at, CEntity* hit) {
    if (s.by && !IsPedValid((CPed*)s.by)) s.by = nullptr;  // the crew is gone (the blast keeps its creator)
    g_hitBy = s.by;
    CVector p = at - s.vel.Normalized() * 0.3f;
    Call<0x736A50>((CEntity*)nullptr, s.by, EXPLOSION_TANK_FIRE, p, 0u, (uint8_t)1, -1.0f, (uint8_t)0);  // CExplosion::AddExplosion
    // the tanks: a hit goes through, a near miss shakes
    if (hit && hit->Type() == ENTITY_TYPE_VEHICLE && hit->ModelIndex() == MODEL_RHINO) {
        SendHit((CVehicle*)hit, ShellDamage((CVehicle*)hit, s.vel), s.pos);
        return;
    }
    CPoolHeader* pool = VehiclePool();
    for (int i = 0; pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* v = (CVehicle*)(pool->objects + (size_t)i * kVehiclePoolElemSize);
        if (v->ModelIndex() == MODEL_RHINO && v != s.from && (v->GetPosition() - p).Length() < 4.0f) SendHit(v, 60.0f, s.pos);
    }
}

void FlyShells(float dt) {
    for (size_t i = 0; i < g_shells.size();) {
        Shell& s = g_shells[i];
        CVector prev = s.pos;
        s.vel.z -= kGravity * dt;
        s.pos = s.pos + s.vel * dt;
        s.age += dt;
        ColPoint cp{};
        CEntity* hit = nullptr;
        Ref<CEntity*>(kIgnoreEntity) = s.from;
        bool struck = Call<0x56BA00, bool>(&prev, &s.pos, &cp, &hit, true, true, true, true, true, true, false, false);  // ProcessLineOfSight
        Ref<CEntity*>(kIgnoreEntity) = nullptr;
        if (struck || s.age > kShellLife) {
            if (struck && s.real) Burst(s, cp.point, hit);
            g_shells.erase(g_shells.begin() + (ptrdiff_t)i);
            continue;
        }
        // seen: a bright point (CCoronas::RegisterCorona, a star) where it is
        Call<0x6FC580>(0x7A4E0000u + (uint32_t)(i & 0xFF), (CEntity*)nullptr, (uint8_t)255, (uint8_t)190, (uint8_t)110, (uint8_t)255,
                       &s.pos, 0.7f, 800.0f, 0, 0, 0, 0, 0, 0.0f, false, 1.5f, false, 15.0f, false, false);
        ++i;
    }
}

void Launch(CVehicle* tank, CEntity* by, const CVector& muzzle, const CVector& vel, bool real) {
    Shell s;
    s.pos = muzzle;
    s.vel = vel;
    s.from = tank;
    s.by = by;
    s.real = real;
    g_shells.push_back(s);
    CVector look = vel * 0.25f;  // the flash along the barrel (Fx_c::TriggerTankFire takes the line to the target)
    CallMethod<0x4A0FA0>((void*)0xA9AE00, &muzzle, &look);  // g_fx.TriggerTankFire
}

// this game fires a tank's gun: the real shell here, its copy in the other games
void Fire(CVehicle* tank, CEntity* by, float spreadDeg) {
    CVector dir = BarrelDir(tank);
    if (spreadDeg > 0.0f) {
        CVector side = dir.Cross(CVector{0, 0, 1}).Normalized(), up = side.Cross(dir).Normalized();
        float a = Rand(-spreadDeg, spreadDeg) * 0.0174533f, b = Rand(-spreadDeg, spreadDeg) * 0.0174533f;
        dir = (dir + side * std::tan(a) + up * std::tan(b)).Normalized();
    }
    CVector muzzle = Muzzle(tank, dir);
    CVector vel = dir * kShellSpeed + tank->MoveSpeed() * 50.0f;
    Launch(tank, by, muzzle, vel, true);
    tank->MoveSpeed() = tank->MoveSpeed() - dir * 0.012f;  // the recoil
    if (Net().Online()) {
        NetVehicle* nv = Vehicles::Instance().Find(tank);
        Writer w(proto::G_TANK_SHOT);
        w.U16(nv ? nv->netId : 0xFFFF);
        w.Vec({muzzle.x, muzzle.y, muzzle.z});
        w.Vec({vel.x, vel.y, vel.z});
        Net().Send(proto::CH_RELIABLE, w);
    }
}

void HandleShot(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    Vec3 m = r.Vec(), v = r.Vec();
    if (!r.Ok() || !core::IsPlayerReady()) return;
    NetVehicle* nv = id != 0xFFFF ? Vehicles::Instance().Find(id) : nullptr;
    Launch(nv && Vehicles::Alive(*nv) ? nv->veh : nullptr, nullptr, {m.x, m.y, m.z}, {v.x, v.y, v.z}, false);
    // where the shell is now: it has flown on while the message came
    Shell& s = g_shells.back();
    float lat = Net().LatencyFrom(from);
    s.pos = s.pos + s.vel * lat;
    s.age = lat;
}

// ---------------------------------------------------------------- damage to a tank of this game
void Hurt(CVehicle* tank, float damage, const CVector& from) {
    if (!IsVehicleValid(tank) || (Field<uint8_t>(tank, 0x36) >> 3) == STATUS_WRECKED) return;
    float& health = tank->Health();
    health -= damage;
    LOGD("Tank hit: -%.0f, health %.0f", damage, health);
    if (health <= 0.0f) {
        health = 0.0f;
        CallMethod<0x6B3780>(tank, (CEntity*)nullptr, false);  // CAutomobile::BlowUpCar
    }
    CPlayerPed* me = FindPlayerPed(0);
    if (me && me->VehicleIfIn() == tank) Call<0x50A970>((void*)addr::TheCamera, std::min(0.4f, damage / 900.0f));  // CamShakeNoPos
}

void HandleHit(uint8_t from, Reader& r) {
    uint16_t id = r.U16();
    float damage = r.F32();
    Vec3 f = r.Vec();
    if (!r.Ok() || !core::IsPlayerReady() || damage <= 0.0f || damage > 2000.0f) return;
    NetVehicle* nv = Vehicles::Instance().Find(id);
    if (!nv || !Vehicles::Alive(*nv) || Vehicles::Instance().IsMirror(*nv) || nv->veh->ModelIndex() != MODEL_RHINO) return;
    Hurt(nv->veh, damage, {f.x, f.y, f.z});
}

// ---------------------------------------------------------------- rockets
// A tank is proof against blasts, a rocket's too. Here a rocket (of a soldier's or a player's launcher) bursting within
// 5 m of a tank takes 30% of its armour (300 of its health), in the game the rocket was fired in: the game's explosions
// (CExplosion::aExplosions, 16 of 0x7C bytes) are looked over each frame for new ones.
constexpr uintptr_t kExplosions = 0xC88950;
constexpr int kExplosionSize = 0x7C;
constexpr int EXPLOSION_ROCKET = 2, EXPLOSION_WEAK_ROCKET = 3;
constexpr float kRocketDamage = 300.0f;
uint32_t g_seenExplosion[16] = {};

// whether a blast is this game's own (the other game's comes here as a copy, its harm is that game's)
bool OursBy(CEntity* e) {
    if (!e) return false;
    if (e == FindPlayerPed(0)) return true;
    if (!e->IsPed() || Players::Instance().FindByPed((CPed*)e)) return false;
    NetPed* np = Peds::Instance().Find((CPed*)e);
    return !np || !Net().Online() || np->owner == Net().MyId();
}

void RocketsOnTanks() {
    for (int i = 0; i < 16; ++i) {
        uintptr_t e = kExplosions + (uintptr_t)i * kExplosionSize;
        int type = Ref<int32_t>(e + 0x00);
        uint32_t created = Ref<uint32_t>(e + 0x2C);  // m_nCreatedTime
        bool active = Ref<uint8_t>(e + 0x28) != 0;   // m_nActiveCounter
        if (!active || created == g_seenExplosion[i]) continue;
        g_seenExplosion[i] = created;
        if (type != EXPLOSION_ROCKET && type != EXPLOSION_WEAK_ROCKET) continue;
        CEntity* by = Ref<CEntity*>(e + 0x18);
        if (!OursBy(by)) continue;
        CVector at = Ref<CVector>(e + 0x04);
        CPoolHeader* pool = VehiclePool();
        for (int k = 0; pool && k < pool->size; ++k) {
            if (pool->byteMap[k] < 0) continue;
            auto* v = (CVehicle*)(pool->objects + (size_t)k * kVehiclePoolElemSize);
            if (v->ModelIndex() != MODEL_RHINO || (v->GetPosition() - at).Length() > 5.0f) continue;
            g_hitBy = by;
            SendHit(v, kRocketDamage, at);
            g_hitBy = nullptr;
        }
    }
}

// ---------------------------------------------------------------- the player's gun
// The game's own shot never comes, for any tank: the time of the last big-gun shot (CPlayerInfo::m_nLastTimeBigGunFired,
// +0xF0, which TankControl checks against 800 ms) is "now" for both player slots while a tank's control runs - the remote
// player's tank copy runs it too, and fired the game's shot there every 800 ms. Our gun: one shell per 4 s.
// Aiming: the turret turns to where the camera looks (a line from the camera), allowing for the shell's drop and the
// tank's own speed - the game's lays it 15 degrees over the camera, right for a blast 60 m away, not for a shell.
// Where the shell would land is worked out each frame (its flight, step by step) and marked on the screen.
uint64_t g_playerLoadedAt = 0;  // when our gun is loaded again
CVector g_impact;               // where our shell would land now
bool g_impactOk = false, g_impactTank = false;
constexpr uint16_t MODE_CAM_ON_A_STRING = 18;

bool LineHit(const CVector& a, const CVector& b, CVehicle* ignore, CVector& at, CEntity** what = nullptr) {
    ColPoint cp{};
    CEntity* hit = nullptr;
    Ref<CEntity*>(kIgnoreEntity) = ignore;
    bool struck = Call<0x56BA00, bool>(&a, &b, &cp, &hit, true, true, true, true, true, true, false, false);  // ProcessLineOfSight
    Ref<CEntity*>(kIgnoreEntity) = nullptr;
    if (struck) at = cp.point;
    if (what) *what = struck ? hit : nullptr;
    return struck;
}

// the turret to the camera's aim: from (yaw0, pitch0), at its pace
void AimTurret(CVehicle* v, float yaw0, float pitch0, float dt) {
    if (cam::Mode() != MODE_CAM_ON_A_STRING) return;  // a look from the bumper: the pad turns it (the game's way)
    CVector front = cam::Front(), src = cam::Source();
    if (front.Length() < 0.5f) return;
    front = front.Normalized();
    float skip = std::max(0.0f, (v->GetPosition() - src).Dot(front)) + 4.0f;  // past the tank itself
    CVector a = src + front * skip, b = src + front * 700.0f, aim = b;
    LineHit(a, b, v, aim);
    CVector muzzle = Muzzle(v, BarrelDir(v));
    float flight = (aim - muzzle).Length() / kShellSpeed;
    aim = aim + CVector{0, 0, 0.5f * kGravity * flight * flight} - v->MoveSpeed() * 50.0f * flight;
    CVector l = Unrotate(v, (aim - muzzle).Normalized());
    float wantYaw = std::atan2(-l.x, l.y), wantPitch = std::clamp(PitchFor(l), -0.12f, 0.61f);
    if (std::fabs(wantYaw) > 1.5708f) wantPitch = std::max(wantPitch, -0.05f);  // over the engine deck
    float dy = Wrap(wantYaw - yaw0), dp = wantPitch - pitch0;
    float stepYaw = 0.95f * dt, stepPitch = 0.4f * dt;  // a little quicker than the game's (0.75 and 0.25)
    Field<float>(v, kTurretYaw) = Wrap(yaw0 + std::clamp(dy, -stepYaw, stepYaw));
    Field<float>(v, kTurretPitch) = pitch0 + std::clamp(dp, -stepPitch, stepPitch);
}

// where a shell fired now would come down
void PredictImpact(CVehicle* v) {
    CVector dir = BarrelDir(v);
    CVector pos = Muzzle(v, dir), vel = dir * kShellSpeed + v->MoveSpeed() * 50.0f;
    g_impactOk = g_impactTank = false;
    constexpr float step = 0.04f;
    for (float t = 0; t < kShellLife; t += step) {
        CVector prev = pos;
        vel.z -= kGravity * step;
        pos = pos + vel * step;
        CEntity* what = nullptr;
        if (LineHit(prev, pos, v, g_impact, &what)) {
            g_impactOk = true;
            g_impactTank = what && what->Type() == ENTITY_TYPE_VEHICLE && what->ModelIndex() == MODEL_RHINO;
            return;
        }
    }
}

// And the gun's button answers nothing inside the game's tank control (CPad::CarGunJustDown, its only way to the
// game's shot): the shot's timer alone did not keep it from firing every click.
bool g_gunMuted = false;
using GunDownFn = int16_t(__fastcall*)(void*, void*);
GunDownFn g_origGunDown = nullptr;
int16_t __fastcall HookCarGunJustDown(void* pad, void* edx) { return g_gunMuted ? 0 : g_origGunDown(pad, edx); }

using TankControlFn = void(__fastcall*)(CVehicle*, void*);
TankControlFn g_origTankControl = nullptr;
void __fastcall HookTankControl(CVehicle* v, void* edx) {
    if (!v || v->ModelIndex() != MODEL_RHINO) return g_origTankControl(v, edx);  // the RC Tiger's gun: as ever
    uint32_t& last0 = Ref<uint32_t>((uintptr_t)PlayerInfo(0) + 0xF0);
    uint32_t& last1 = Ref<uint32_t>((uintptr_t)PlayerInfo(1) + 0xF0);
    uint32_t saved0 = last0, saved1 = last1;
    last0 = last1 = TimeMs();  // the game's own shot never comes (CPlayerInfo::m_nLastTimeBigGunFired)
    CPlayerPed* me = FindPlayerPed(0);
    bool mine = me && v->Driver() == me && v->Status() == 0 /* STATUS_PLAYER */;
    float yaw0 = Field<float>(v, kTurretYaw), pitch0 = Field<float>(v, kTurretPitch);
    bool fire = mine && CallMethod<0x53FFE0, int16_t>(GetPad(0)) != 0;  // CPad::CarGunJustDown
    g_gunMuted = true;
    g_origTankControl(v, edx);
    g_gunMuted = false;
    last0 = saved0, last1 = saved1;  // the planes' guns use it too
    if (!mine) return;
    AimTurret(v, yaw0, pitch0, std::clamp(TimeStep() / 50.0f, 0.0f, 0.1f));
    if (fire && NowMs() >= g_playerLoadedAt) {
        g_playerLoadedAt = NowMs() + kReloadPlayerMs;
        Fire(v, me, 0.0f);
        Call<0x50A970>((void*)addr::TheCamera, 0.12f);  // CamShakeNoPos: the shot
    }
}

// ---------------------------------------------------------------- crews
struct Crew {
    uint64_t loadedAt = 0;
};
std::map<CVehicle*, Crew> g_crews;
std::set<CVehicle*> g_battleTanks;  // the battle's enemy tanks (this game's)
std::set<CPed*> g_battleSoldiers;   // and its soldiers with rocket launchers

struct Target {
    CVector pos, vel;
    bool ok = false;
};

uint8_t LocalWanted() {
    void* w = Call<0x56E230, void*>(-1);  // FindPlayerWanted
    return w ? (uint8_t)std::clamp(Field<int32_t>(w, 0x2C), 0, 6) : 0;
}

// the player this crew shoots at: the nearest in reach it is after
Target PickTarget(CVehicle* tank, bool anyPlayer) {
    Target best;
    float bestD = kCrewRange;
    CVector at = tank->GetPosition();
    auto consider = [&](CPed* ped, uint8_t wanted, bool dead) {
        if (!ped || dead || (!anyPlayer && wanted < 4) || ped->AreaCode() != tank->AreaCode()) return;
        CVehicle* in = ped->VehicleIfIn();
        CVector p = in ? in->GetPosition() : ped->GetPosition() + CVector{0, 0, 0.5f};
        float d = (p - at).Length();
        if (d < 12.0f || d >= bestD) return;
        bestD = d;
        best.pos = p;
        best.vel = (in ? in->MoveSpeed() : ped->MoveSpeed()) * 50.0f;
        best.ok = true;
    };
    if (CPlayerPed* me = FindPlayerPed(0)) consider(me, LocalWanted(), me->Health() <= 0.0f);
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.ped && rp.hasState) consider(rp.ped, rp.state.wanted, (rp.state.flags & OnFootState::F_DEAD) != 0);
    return best;
}

void CrewAims(CVehicle* tank, Crew& crew, bool anyPlayer, float dt) {
    Target t = PickTarget(tank, anyPlayer);
    if (!t.ok) return;
    CVector dir0 = BarrelDir(tank);
    CVector muzzle = Muzzle(tank, dir0);
    // lead the target and allow for the drop over the flight
    float flight = (t.pos - muzzle).Length() / kShellSpeed;
    CVector aim = t.pos + t.vel * flight + CVector{0, 0, 0.5f * kGravity * flight * flight};
    CVector l = Unrotate(tank, (aim - muzzle).Normalized());
    float wantYaw = std::atan2(-l.x, l.y), wantPitch = PitchFor(l);
    wantPitch = std::clamp(wantPitch, -0.12f, 0.61f);  // the gun's own limits (-7..35 degrees)
    float& yaw = Field<float>(tank, kTurretYaw);
    float& pitch = Field<float>(tank, kTurretPitch);
    float dy = Wrap(wantYaw - yaw), dp = wantPitch - pitch;
    float stepYaw = 0.75f * dt, stepPitch = 0.25f * dt;  // radians a second: as a player's turret turns
    yaw = Wrap(yaw + std::clamp(dy, -stepYaw, stepYaw));
    pitch += std::clamp(dp, -stepPitch, stepPitch);
    if (std::fabs(dy) > 0.035f || std::fabs(dp) > 0.03f || NowMs() < crew.loadedAt) return;
    // the target in sight (nothing of the map between)
    if (!Call<0x56A490, bool>(&muzzle, &t.pos, true, false, false, true, false, false, false)) return;  // GetIsLineOfSightClear
    crew.loadedAt = NowMs() + kReloadCrewMs + (uint64_t)Rand(0, 1500);
    Fire(tank, tank->Driver(), 0.9f);
}

void UpdateCrews(float dt) {
    CPoolHeader* pool = VehiclePool();
    std::set<CVehicle*> seen;
    for (int i = 0; pool && i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* v = (CVehicle*)(pool->objects + (size_t)i * kVehiclePoolElemSize);
        if (v->ModelIndex() != MODEL_RHINO || (Field<uint8_t>(v, 0x36) >> 3) == STATUS_WRECKED) continue;
        CPed* d = v->Driver();
        if (!d || d->IsPlayer() || Players::Instance().FindByPed(d) || d->Health() <= 0.0f) continue;
        if (NetVehicle* nv = Vehicles::Instance().Find(v); nv && Vehicles::Instance().IsMirror(*nv)) continue;  // its game aims it
        bool battle = g_battleTanks.count(v) != 0;
        if (!battle && d->PedType() != PED_TYPE_COP) continue;  // a mission's tank: its script decides
        seen.insert(v);
        CrewAims(v, g_crews[v], battle, dt);
    }
    for (auto it = g_crews.begin(); it != g_crews.end();) it = seen.count(it->first) ? std::next(it) : g_crews.erase(it);
}

// ---------------------------------------------------------------- the tank battle
enum BattleOp : uint8_t { B_START = 1, B_WAVE, B_END, B_JOIN, B_LEFT };
constexpr int kWaves = 3;
constexpr float kBattleRadius = 400.0f;
struct Battle {
    bool on = false;
    bool host = false;
    CVector centre;
    int wave = 0;
    uint64_t startMs = 0, nextWaveMs = 0, allDeadSince = 0;
    std::vector<uint16_t> enemies;  // the enemy tanks (net ids, for the radar)
    int left = 0;                   // enemies left in the wave (the host counts its own tanks and soldiers)
    std::set<uint8_t> players;      // who is in it (the host's list)
    bool joined = false;            // this player is in it
    int myTank = -1;                // the Rhino this game made for its player
} g_battle;

CVector RoadNear(const CVector& p) {
    float x = 0, y = 0, z = 0;
    Cmd(0x02C1).F(p.x).F(p.y).F(p.z).Out(&x).Out(&y).Out(&z).Run();  // GET_CLOSEST_CAR_NODE
    return (x || y) ? CVector{x, y, z} : p;
}

void SendBattle(BattleOp op, const std::function<void(Writer&)>& body = nullptr) {
    Writer w(proto::G_TANK_BATTLE);
    w.U8(op);
    if (body) body(w);
    Net().Send(proto::CH_RELIABLE, w);
}

// this player into a tank by the road (its own game's)
void JoinBattle() {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || g_battle.joined || me->AreaCode() != 0 || (me->GetPosition() - g_battle.centre).Length2D() > kBattleRadius) return;
    g_battle.joined = true;
    game::challenge::Begin();
    if (Net().IsHost()) g_battle.players.insert(Net().MyId());
    else SendBattle(B_JOIN);
    CVehicle* in = me->VehicleIfIn();
    if (in && in->ModelIndex() == MODEL_RHINO) {
        ui::TheChat().AddSystem(TR("Танковое сражение! Вы уже в танке.", "A tank battle! You are in a tank already."));
        return;
    }
    if (!LoadModelNow(MODEL_RHINO)) return;
    int h = GetPedRef(me);
    CVector p = me->GetPosition();
    if (in) Cmd(0x0362).I(h).F(p.x).F(p.y).F(p.z + 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
    CVector at = RoadNear(p);
    int car = -1;
    Cmd(0x00A5).I(MODEL_RHINO).F(at.x).F(at.y).F(at.z + 1.0f).Out(&car).Run();  // CREATE_CAR
    if (car < 0) return;
    CVector toward = g_battle.centre - at;
    Cmd(0x0175).I(car).F(std::atan2(-toward.x, toward.y) * 57.29578f).Run();  // SET_CAR_HEADING
    Cmd(0x0224).I(car).I(1500).Run();                                           // SET_CAR_HEALTH
    Cmd(0x036A).I(h).I(car).Run();                                              // WARP_CHAR_INTO_CAR
    g_battle.myTank = car;
    MarkModelNotNeeded(MODEL_RHINO);
    ui::TheChat().AddSystem(TR("Танковое сражение! Вы в танке. ЛКМ — выстрел (перезарядка 4 с).",
                               "A tank battle! You are in a tank. Fire with the left mouse button (4 s to reload)."));
}

void StartBattle() {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || me->AreaCode() != 0) {
        ui::TheChat().AddError(TR("Танковое сражение начинается только на улице.", "A tank battle starts only outdoors."));
        return;
    }
    g_battle = Battle{};
    g_battle.on = g_battle.host = true;
    g_battle.centre = me->GetPosition();
    g_battle.startMs = NowMs();
    g_battle.nextWaveMs = NowMs() + 12000;
    CVector c = g_battle.centre;
    SendBattle(B_START, [c](Writer& w) { w.Vec({c.x, c.y, c.z}); });
    JoinBattle();
    LOGI("Tank battle started at %.0f, %.0f", c.x, c.y);
}

// soldiers with rocket launchers, on foot, after the players: a rocket in a tank takes 30% of its armour
constexpr int MODEL_ROCKETLA = 359, WEAPON_RLAUNCHER = 35;
void SpawnSoldiers(int count, float side) {
    if (!LoadModelNow(MODEL_ROCKETLA)) return;
    for (int k = 0; k < count; ++k) {
        float a = side + Rand(-0.7f, 0.7f), d = Rand(110.0f, 150.0f);
        CVector p = g_battle.centre + CVector{std::cos(a) * d, std::sin(a) * d, 0.0f};
        float x = 0, y = 0, z = 0;
        Cmd(0x02C0).F(p.x).F(p.y).F(p.z).Out(&x).Out(&y).Out(&z).Run();  // GET_CLOSEST_CHAR_NODE
        if (x || y) p = {x, y, z};
        int h = -1;
        if (!HasRoom(PedPool(), kKeepPeds + 2, "ped")) break;
        Cmd(0x009A).I(PED_TYPE_MISSION1).I(MODEL_ARMY).F(p.x).F(p.y).F(p.z + 1.0f).Out(&h).Run();  // CREATE_CHAR
        if (h < 0) continue;
        Cmd(0x087E).I(h).I(0).Run();                       // SET_CHAR_DROPS_WEAPONS_WHEN_DEAD: none
        if (CPed* made = GetPed(h)) Peds::Instance().Share(made);  // the others see them (no mission made them)
        Cmd(0x01B2).I(h).I(WEAPON_RLAUNCHER).I(40).Run();  // GIVE_WEAPON_TO_CHAR
        Cmd(0x01B9).I(h).I(WEAPON_RLAUNCHER).Run();        // SET_CURRENT_CHAR_WEAPON
        Cmd(0x0223).I(h).I(150).Run();                     // SET_CHAR_HEALTH
        Cmd(0x02E2).I(h).I(55).Run();                      // SET_CHAR_ACCURACY
        Cmd(0x060F).I(h).F(400.0f).Run();                  // SET_SENSE_RANGE: it knows where the players are
        Cmd(0x060B).I(h).I(65542).Run();                   // SET_CHAR_DECISION_MAKER "Empty": no fear
        if (CPed* ped = GetPed(h)) g_battleSoldiers.insert(ped);
    }
    MarkModelNotNeeded(MODEL_ROCKETLA);
}

// the player ped (this game's own or a copy) nearest to a place, of those in the battle and alive
CPed* NearestPlayer(const CVector& at) {
    CPed* best = nullptr;
    float bestD = 1e9f;
    auto consider = [&](CPed* ped, bool dead) {
        if (!ped || dead || ped->AreaCode() != 0) return;
        float d = (ped->GetPosition() - at).Length();
        if (d < bestD) bestD = d, best = ped;
    };
    if (CPlayerPed* me = FindPlayerPed(0); me && g_battle.joined) consider(me, me->Health() <= 0.0f);
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.ped && rp.hasState && g_battle.players.count(id)) consider(rp.ped, (rp.state.flags & OnFootState::F_DEAD) != 0);
    return best;
}

void SpawnWave() {
    ++g_battle.wave;
    int count = std::min(8, (int)std::max<size_t>(1, g_battle.players.size()) + g_battle.wave);
    if (!LoadModelNow(MODEL_RHINO) || !LoadModelNow(MODEL_ARMY)) return;
    g_battle.enemies.clear();
    float base = Rand(0.0f, 6.2831853f);
    SpawnSoldiers(1 + g_battle.wave, base);  // 2, 3, 4 a wave
    for (int k = 0; k < count; ++k) {
        float a = base + Rand(-0.5f, 0.5f) + (k % 2 ? 0.6f : -0.6f) * (k / 2);  // from one side, spread out
        float d = Rand(250.0f, 350.0f);
        CVector at = RoadNear(g_battle.centre + CVector{std::cos(a) * d, std::sin(a) * d, 0.0f});
        int car = -1, crew = -1;
        Cmd(0x00A5).I(MODEL_RHINO).F(at.x).F(at.y).F(at.z + 1.0f).Out(&car).Run();  // CREATE_CAR
        if (car < 0) continue;
        CVector toward = g_battle.centre - at;
        Cmd(0x0175).I(car).F(std::atan2(-toward.x, toward.y) * 57.29578f).Run();        // SET_CAR_HEADING
        Cmd(0x0129).I(car).I(PED_TYPE_MISSION1).I(MODEL_ARMY).Out(&crew).Run();         // CREATE_CHAR_INSIDE_CAR
        Cmd(0x020A).I(car).I(2).Run();                                                   // LOCK_CAR_DOORS: locked
        if (crew >= 0) Cmd(0x039E).I(crew).I(1).Run();                                   // SET_CHAR_CANT_BE_DRAGGED_OUT
        if (crew >= 0) Cmd(0x060B).I(crew).I(65542).Run();                               // SET_CHAR_DECISION_MAKER: no bailing out
        if (CPed* made = crew >= 0 ? GetPed(crew) : nullptr) Peds::Instance().Share(made);
        Cmd(0x00AD).I(car).F(12.0f).Run();                                               // SET_CAR_CRUISE_SPEED
        Cmd(0x00AE).I(car).I(2).Run();                                                   // SET_CAR_DRIVING_STYLE: around cars
        CVehicle* v = GetVehicle(car);
        if (!v) continue;
        g_battleTanks.insert(v);
        if (NetVehicle* nv = Vehicles::Instance().EnsureNetworked(v)) g_battle.enemies.push_back(nv->netId);
    }
    MarkModelNotNeeded(MODEL_ARMY);
    std::vector<uint16_t> ids = g_battle.enemies;
    int wave = g_battle.wave;
    SendBattle(B_WAVE, [&](Writer& w) {
        w.U8((uint8_t)wave);
        w.U8((uint8_t)ids.size());
        for (uint16_t id : ids) w.U16(id);
    });
    ui::TheChat().AddSystem(Format(TR("Волна %d из %d: танков противника — %d.", "Wave %d of %d: %d enemy tanks."), wave, kWaves, (int)ids.size()));
    LOGI("Tank battle: wave %d, %d tanks", wave, (int)ids.size());
}

// an enemy tank fights on while it is whole and its crew alive in it
bool TankFights(CVehicle* v) {
    if (!IsVehicleValid(v) || (Field<uint8_t>(v, 0x36) >> 3) == STATUS_WRECKED || v->Health() <= 0.0f) return false;
    CPed* d = v->Driver();
    return d && d->Health() > 0.0f;
}
bool SoldierFights(CPed* p) { return IsPedValid(p) && p->Health() > 0.0f; }

// the host counts this game's own tanks and soldiers (a tank made far from the others has no net id at all, and
// counting by net ids let a wave pass at once); the others are told the count
int CountLeft() {
    int n = 0;
    for (CVehicle* v : g_battleTanks) n += TankFights(v);
    for (CPed* p : g_battleSoldiers) n += SoldierFights(p);
    return n;
}
int EnemiesLeft() { return g_battle.left; }

void EmptyStreets(bool on);

void EndBattle(bool won, bool tell) {
    if (!g_battle.on) return;
    if (tell && g_battle.host) SendBattle(B_END, [won](Writer& w) { w.U8(won); });
    if (g_battle.host) {  // the enemies left go: their crews out, the wrecks stay
        for (CVehicle* v : g_battleTanks)
            if (IsVehicleValid(v)) {
                if (CPed* d = v->Driver()) Cmd(0x009B).I(GetPedRef(d)).Run();  // DELETE_CHAR
                Cmd(0x01C3).I(GetVehicleRef(v)).Run();                           // MARK_CAR_AS_NO_LONGER_NEEDED
            }
        for (CPed* p : g_battleSoldiers)
            if (IsPedValid(p)) Cmd(p->Health() > 0.0f ? 0x009B : 0x01C2).I(GetPedRef(p)).Run();  // DELETE_CHAR / no longer needed
    }
    g_battleTanks.clear();
    g_battleSoldiers.clear();
    if (g_battle.joined) {
        if (won) {
            int prize = 2000 + 1000 * kWaves;
            PlayerInfos()[0].money += prize;
            ui::TheChat().AddSystem(Format(TR("Победа в танковом сражении! Награда $%d.", "The tank battle is won! Prize $%d."), prize));
        } else {
            ui::TheChat().AddSystem(TR("Танковое сражение окончено.", "The tank battle is over."));
        }
        if (g_battle.myTank >= 0 && GetVehicle(g_battle.myTank)) Cmd(0x01C3).I(g_battle.myTank).Run();  // an ordinary car now
        game::challenge::End();
    }
    for (int k = 0; k < 16; ++k) radar::RemoveIcon(9600 + k);
    EmptyStreets(false);
    LOGI("Tank battle over (%s)", won ? "won" : "lost");
    g_battle = Battle{};
}

void HandleBattle(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    if (!r.Ok() || !core::IsPlayerReady()) return;
    if (op == B_START) {
        Vec3 c = r.Vec();
        if (!r.Ok() || from != Net().HostId()) return;
        g_battle = Battle{};
        g_battle.on = true;
        g_battle.centre = {c.x, c.y, c.z};
        g_battle.startMs = NowMs();
        CPlayerPed* me = FindPlayerPed(0);
        if (me && me->AreaCode() == 0 && (me->GetPosition() - g_battle.centre).Length2D() < kBattleRadius) JoinBattle();
        else
            ui::TheChat().AddSystem(Format(TR("%s начал танковое сражение. Будьте рядом с ним (до 400 м) и напишите /tanks join.",
                                              "%s started a tank battle. Come within 400 m of the host and type /tanks join."),
                                           Net().PeerName(from).c_str()));
    } else if (op == B_WAVE) {
        uint8_t wave = r.U8(), n = r.U8();
        std::vector<uint16_t> ids;
        for (int k = 0; k < n; ++k) ids.push_back(r.U16());
        if (!r.Ok() || from != Net().HostId() || !g_battle.on) return;
        g_battle.wave = wave;
        g_battle.enemies = ids;
        ui::TheChat().AddSystem(Format(TR("Волна %d из %d: танков противника — %d.", "Wave %d of %d: %d enemy tanks."), wave, kWaves, (int)n));
    } else if (op == B_END) {
        uint8_t won = r.U8();
        if (r.Ok() && from == Net().HostId()) EndBattle(won != 0, false);
    } else if (op == B_JOIN) {
        if (g_battle.on && g_battle.host) g_battle.players.insert(from);
    } else if (op == B_LEFT) {
        uint8_t n = r.U8();
        if (r.Ok() && from == Net().HostId() && g_battle.on) g_battle.left = n;
    }
}

// The streets of a battle: no passers-by and no traffic made (densities 0, set again every second: a script may set
// its own), and this game's own random people and traffic cleared away (not the players', not the copies of the other
// games' - those games clear their own); the parked and the empty cars stay. Back to as ever when it ends.
bool g_emptied = false;
void EmptyStreets(bool on) {
    if (!on) {
        if (!g_emptied) return;
        g_emptied = false;
        Cmd(0x03DE).F(1.0f).Run();  // SET_PED_DENSITY_MULTIPLIER
        Cmd(0x01EB).F(1.0f).Run();  // SET_CAR_DENSITY_MULTIPLIER
        LOGI("Tank battle: the streets fill again");
        return;
    }
    if (!g_emptied) LOGI("Tank battle: the streets are emptied");
    g_emptied = true;
    Cmd(0x03DE).F(0.0f).Run();
    Cmd(0x01EB).F(0.0f).Run();
    CPlayerPed* me = FindPlayerPed(0);
    int peds = 0, cars = 0;
    if (CPoolHeader* pool = VehiclePool())
        for (int i = 0; i < pool->size; ++i) {
            if (pool->byteMap[i] < 0) continue;
            auto* v = (CVehicle*)(pool->objects + (size_t)i * kVehiclePoolElemSize);
            if (v->CreatedBy() != 1 /* RANDOM_VEHICLE */ || !v->Driver() || g_battleTanks.count(v) || (me && me->VehicleIfIn() == v)) continue;  // the traffic
            if (NetVehicle* nv = Vehicles::Instance().Find(v); nv && Vehicles::Instance().IsMirror(*nv)) continue;  // another game's
            bool players = false;
            auto isPlayer = [](CPed* q) { return q && (q->IsPlayer() || Players::Instance().FindByPed(q) != nullptr); };
            players |= isPlayer(v->Driver());
            for (int k = 0; k < 8; ++k) players |= isPlayer(v->Passengers()[k]);
            if (players) continue;
            Cmd(0x00A6).I(GetVehicleRef(v)).Run();  // DELETE_CAR (its people with it)
            ++cars;
        }
    if (CPoolHeader* pool = PedPool())
        for (int i = 0; i < pool->size; ++i) {
            if (pool->byteMap[i] < 0) continue;
            auto* p = (CPed*)(pool->objects + (size_t)i * kPedPoolElemSize);
            if (p->IsPlayer() || p->CreatedBy() != 1 /* PED_GAME */ || p->InVehicle() || Players::Instance().FindByPed(p)) continue;
            if (NetPed* np = Peds::Instance().Find(p); np && np->owner != Net().MyId()) continue;  // another game's
            if (!CallMethod<0x5DF100, bool>(p)) continue;  // CPed::CanBeDeleted
            Call<0x610F20>(p);  // CPopulation::RemovePed
            ++peds;
        }
    if (peds || cars) LOGD("Tank battle: %d people and %d cars cleared away", peds, cars);
}

// where the enemy tanks are (the host: its own; the others: by their net ids)
std::vector<CVector> EnemyTanks() {
    std::vector<CVector> at;
    if (g_battle.host) {
        for (CVehicle* v : g_battleTanks)
            if (TankFights(v)) at.push_back(v->GetPosition());
    } else {
        for (uint16_t id : g_battle.enemies) {
            NetVehicle* nv = Vehicles::Instance().Find(id);
            if (nv && Vehicles::Alive(*nv) && (Field<uint8_t>(nv->veh, 0x36) >> 3) != STATUS_WRECKED) at.push_back(nv->veh->GetPosition());
        }
    }
    return at;
}

void UpdateBattle() {
    if (!g_battle.on) return;
    static uint64_t emptyMs = 0;
    if ((g_battle.joined || g_battle.host) && NowMs() - emptyMs > 1000) {
        emptyMs = NowMs();
        EmptyStreets(true);
    }
    // the enemies on the radar of everyone in it (the host: its own tanks and soldiers)
    std::vector<CVector> marks = EnemyTanks();
    if (g_battle.host)
        for (CPed* p : g_battleSoldiers)
            if (SoldierFights(p)) marks.push_back(p->GetPosition());
    for (int k = 0; k < 16; ++k) {
        if (k < (int)marks.size()) radar::SetIcon(9600 + k, marks[k], 19 /* ENEMYATTACK */);
        else radar::RemoveIcon(9600 + k);
    }
    if (!g_battle.host) return;
    uint64_t now = NowMs();
    int left = CountLeft();
    if (left != g_battle.left) {
        g_battle.left = left;
        SendBattle(B_LEFT, [left](Writer& w) { w.U8((uint8_t)std::min(left, 255)); });
    }
    if (now - g_battle.startMs > 15 * 60 * 1000) return EndBattle(false, true);
    // the enemies drive in to 140 m of the nearest player of the battle and fight from there (every 3 s)
    static uint64_t driveMs = 0;
    if (now - driveMs > 3000) {
        driveMs = now;
        for (CVehicle* v : g_battleTanks) {
            if (!IsVehicleValid(v) || !v->Driver()) continue;
            Target t = PickTarget(v, true);
            if (!t.ok) continue;
            int h = GetVehicleRef(v);
            if ((t.pos - v->GetPosition()).Length() > 140.0f) Cmd(0x00A7).I(h).F(t.pos.x).F(t.pos.y).F(t.pos.z).Run();  // CAR_GOTO_COORDINATES
            else Cmd(0x00A9).I(h).Run();                                                                                  // CAR_SET_IDLE
        }
        static std::map<CPed*, std::pair<CPed*, uint64_t>> after;  // whom each soldier is after, since when
        for (CPed* p : g_battleSoldiers) {
            if (!SoldierFights(p)) continue;
            CPed* target = NearestPlayer(p->GetPosition());
            if (!target || (after[p].first == target && now - after[p].second < 9000)) continue;
            after[p] = {target, now};
            Cmd(0x05E2).I(GetPedRef(p)).I(GetPedRef(target)).Run();  // TASK_KILL_CHAR_ON_FOOT (given again now and then)
        }
        for (auto it = after.begin(); it != after.end();) it = g_battleSoldiers.count(it->first) ? std::next(it) : after.erase(it);
    }
    // everyone in it dead: lost
    bool anyAlive = false;
    for (uint8_t id : g_battle.players) {
        if (id == Net().MyId()) {
            CPlayerPed* me = FindPlayerPed(0);
            anyAlive |= me && me->Health() > 0.0f;
        } else if (const RemotePlayer* rp = Players::Instance().Get(id); rp && rp->hasState) {
            anyAlive |= !(rp->state.flags & OnFootState::F_DEAD) && NowMs() - rp->stateRecvMs < 5000;
        }
    }
    if (!anyAlive && !g_battle.players.empty()) {
        if (!g_battle.allDeadSince) g_battle.allDeadSince = now;
        if (now - g_battle.allDeadSince > 4000) return EndBattle(false, true);
    } else {
        g_battle.allDeadSince = 0;
    }
    if (g_battle.wave == 0 || left == 0) {
        if (g_battle.wave >= kWaves) return EndBattle(true, true);
        if (!g_battle.nextWaveMs) {
            g_battle.nextWaveMs = now + 8000;
            if (g_battle.wave > 0) ui::TheChat().AddSystem(TR("Волна отбита! Следующая через 8 секунд.", "The wave is beaten! The next one in 8 seconds."));
        }
        if (now >= g_battle.nextWaveMs) {
            g_battle.nextWaveMs = 0;
            SpawnWave();
        }
    }
}

// ---------------------------------------------------------------- the module
class Tanks : public Module {
public:
    const char* Name() const override { return "Tanks"; }
    void OnInstallHooks() override {
        mem::HookFn(0x6AE850, &HookTankControl, &g_origTankControl, "CAutomobile::TankControl");
        mem::HookFn(0x53FFE0, &HookCarGunJustDown, &g_origGunDown, "CPad::CarGunJustDown");
        Net().SetHandler(proto::G_TANK_SHOT, &HandleShot);
        Net().SetHandler(proto::G_TANK_HIT, &HandleHit);
        Net().SetHandler(proto::G_TANK_BATTLE, &HandleBattle);
    }
    void OnGameShutdown() override {
        if (g_battle.joined) game::challenge::End();
        g_shells.clear();
        g_crews.clear();
        g_battleTanks.clear();
        g_battleSoldiers.clear();
        g_battle = Battle{};
        g_impactOk = false;
        for (uint32_t& t : g_seenExplosion) t = 0;
    }
    void OnSessionEnd() override { EndBattle(false, false); }
    void OnPostProcess() override {
        if (!core::IsPlayerReady()) return;
        float dt = std::clamp(TimeStep() / 50.0f, 0.0f, 0.1f);
        if (!g_shells.empty()) FlyShells(dt);
        if (g_battle.joined) game::challenge::Tick();
        RocketsOnTanks();
        UpdateCrews(dt);
        CPlayerPed* me = FindPlayerPed(0);
        CVehicle* in = me ? me->VehicleIfIn() : nullptr;
        if (in && in->ModelIndex() == MODEL_RHINO && in->Driver() == me) PredictImpact(in);
        else g_impactOk = false;
        if (Net().Online()) UpdateBattle();
    }
    void OnRender() override {
        if (MenuActive()) return;
        auto& g = ui::Gfx();
        float s = g.Scale();
        CPlayerPed* me = FindPlayerPed(0);
        CVehicle* v = me ? me->VehicleIfIn() : nullptr;
        if (v && v->ModelIndex() == MODEL_RHINO && v->Driver() == me) {
            uint64_t now = NowMs();
            bool ready = now >= g_playerLoadedAt;
            // where the shell would land: a sight there, red over a tank
            float sx = 0, sy = 0, depth = 0;
            if (g_impactOk && WorldToScreen(g_impact, sx, sy, depth)) {
                uint32_t c = g_impactTank ? 0xFFFF4040 : ready ? 0xFF7FE07F : 0xFFFFB050;
                float r = 9 * s, gap = 4 * s, t = 2 * s;
                g.Line(sx - r - gap, sy, sx - gap, sy, t, c);
                g.Line(sx + gap, sy, sx + r + gap, sy, t, c);
                g.Line(sx, sy - r - gap, sx, sy - gap, t, c);
                g.Line(sx, sy + gap, sx, sy + r + gap, t, c);
                g.Ellipse(sx, sy, 2 * s, 2 * s, c, 12);
                float dist = (g_impact - me->GetPosition()).Length();
                g.Text(ui::FONT_SMALL, sx, sy + r + gap + 2 * s, Format(TR("%.0f м", "%.0f m"), dist), c, ui::TEXT_CENTER | ui::TEXT_OUTLINE);
            }
            std::string gun = ready ? TR("ОРУДИЕ ЗАРЯЖЕНО", "GUN LOADED")
                                    : Format(TR("ПЕРЕЗАРЯДКА %.1f С", "RELOADING %.1f S"), (g_playerLoadedAt - now) / 1000.0f);
            float y = g.Height() * 0.86f;
            g.Text(ui::FONT_NORMAL, g.Width() * 0.5f, y, gun, ready ? 0xFF7FE07F : 0xFFFFB050, ui::TEXT_CENTER | ui::TEXT_OUTLINE);
            float armour = std::clamp(v->Health() / 10.0f, 0.0f, 150.0f);
            g.Text(ui::FONT_SMALL, g.Width() * 0.5f, y + g.LineHeight(ui::FONT_NORMAL), Format(TR("Броня %.0f%%", "Armour %.0f%%"), armour),
                   armour > 40 ? 0xE0FFFFFF : 0xFFFF6060, ui::TEXT_CENTER | ui::TEXT_OUTLINE);
        }
        if (g_battle.on && (g_battle.joined || g_battle.host)) ui::DrawWorldMarks(EnemyTanks(), 0xE0FF3030, 3.2f, 1.3f);
        if (g_battle.on) {
            std::string line = g_battle.wave ? Format(TR("ТАНКОВОЕ СРАЖЕНИЕ · ВОЛНА %d/%d · ВРАГОВ: %d", "TANK BATTLE · WAVE %d/%d · ENEMIES: %d"),
                                                      g_battle.wave, kWaves, EnemiesLeft())
                                             : std::string(TR("ТАНКОВОЕ СРАЖЕНИЕ · ПРОТИВНИК НА ПОДХОДЕ", "TANK BATTLE · THE ENEMY IS COMING"));
            g.Text(ui::FONT_NORMAL, g.Width() * 0.5f, 14 * s, line, 0xFFFFD060, ui::TEXT_CENTER | ui::TEXT_OUTLINE);
        }
    }
    bool OnCommand(const std::string& cmd, const std::vector<std::string>& args) override {
        if (cmd != "tanks" && cmd != "tankbattle") return false;
        std::string a = args.empty() ? "" : ToLower(args[0]);
        if (a == "join") {
            if (!g_battle.on) ui::TheChat().AddError(TR("Сражение не идёт.", "No battle is on."));
            else JoinBattle();
            return true;
        }
        if (!Net().Online() || !Net().IsHost()) {
            ui::TheChat().AddError(TR("Танковое сражение начинает хост.", "The host starts a tank battle."));
            return true;
        }
        if (a == "stop") {
            EndBattle(false, true);
            return true;
        }
        if (g_battle.on) {
            ui::TheChat().AddError(TR("Сражение уже идёт (/tanks stop — закончить).", "A battle is on already (/tanks stop ends it)."));
            return true;
        }
        StartBattle();
        ui::TheChat().AddSystem(TR("Танковое сражение: первая волна через 12 секунд!", "A tank battle: the first wave in 12 seconds!"));
        return true;
    }
};

Tanks g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

}  // namespace coop
