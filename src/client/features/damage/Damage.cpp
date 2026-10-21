// Damage between the games. Who decides a hit:
//  * NPCs and vehicles are damaged only by their owner's game: a hit of the local player on a mirror is
//    sent to the owner, which applies it with the game's own damage functions (the NPC then reacts to
//    the remote player as the attacker: flees, fights back, dies);
//  * shots of the remote player are only replayed here (pad emulation): their damage is dropped, the
//    shooter's game reports its hits itself. Vehicle collisions are the exception: they are simulated
//    by the victim's owner anyway, so they are never forwarded;
//  * everything else (NPC shots, explosions, fire) hurts whatever this game owns, the local player too;
//  * the other player's blasts go off here as well (Fire.cpp) and hurt the peds this game owns: its copies of them are
//    proof against blasts in the shooter's game. Cars take a blast there and the hit is forwarded, as a bullet's;
//  * an NPC's punch or blade on the remote player is sent to that player's game: the remote ped here has every proof,
//    and the NPC's mirror there only plays the animation (its gunfire needs nothing: the mirror there shoots for real);
//  * a ped killed by the other player drops its money and weapon in the killer's game, where the killer is;
//  * a crime is charged by the game of whoever committed it: the owner keeps its own player's wanted level when it
//    applies the other player's hit, and the hitter's game reports the crime itself;
//  * a kill counts (stats, respect, rampages, gang war provocation) in the killer's game: the owner of the ped tells it.
//    The gang wars run in the host's game only, so the partner's kills of the enemy gangs near the host provoke them there;
//  * the players hurt each other only with friendly fire (the host's choice). The shooter's game reports its bullets
//    and blows on the other player (it sees them hit), the victim's game lets the other player's explosions, fire and
//    car hurt its player (the copy here is proof against them, so they never reach the shooter's game);
//  * during a scripted scene (the camera's wide screen) the other player's side hurts nothing here: its player keeps
//    playing while this one watches, and a mission failed when its people got hurt in the scene.
#include <algorithm>
#include <cmath>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/ped/Peds.h"
#include "entities/player/Players.h"
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

constexpr int WEAPON_LAST_MELEE = 15, WEAPON_PISTOL = 22, WEAPON_SNIPERRIFLE = 34, WEAPON_MINIGUN = 38,
              WEAPON_SPRAYCAN = 41, WEAPON_EXTINGUISHER = 42, WEAPON_RAMMEDBYCAR = 49, WEAPON_RUNOVERBYCAR = 50,
              WEAPON_EXPLOSION = 51, WEAPON_UZI_DRIVEBY = 52;
// TARGET_PLAYER: an NPC hit the player (netId is the NPC), TARGET_PLAYER_BY_PLAYER: the sender hit the player itself,
// KILLED_BY_YOU: the receiver's hit killed the sender's ped (piece is the headshot), GANG_KILL: the partner killed an
// enemy gang member (weapon is its ped type) for the host's gang wars
enum : uint8_t { TARGET_PED = 1, TARGET_VEHICLE = 2, TARGET_PLAYER = 3, TARGET_PLAYER_BY_PLAYER = 4, KILLED_BY_YOU = 5, GANG_KILL = 6 };
constexpr int CRIME_DAMAGED_PED = 2, CRIME_DAMAGED_COP = 3;

// The engine charges a crime to the player in focus (CEventGroup::Add -> ReportCriminalEvent -> FindPlayerWanted):
// the other player's hits, applied here, raised this player's wanted level. It is put back as it was (CWanted up to
// its police scanner, an audio entity left alone).
struct KeepWanted {
    uint8_t* wanted = nullptr;
    uint8_t saved[0x21C];
    KeepWanted() {
        CPlayerPed* me = FindPlayerPed(0);
        if (me && me->PlayerData()) wanted = (uint8_t*)me->Wanted();
        if (wanted) memcpy(saved, wanted, sizeof saved);
    }
    ~KeepWanted() {
        if (wanted) memcpy(wanted, saved, sizeof saved);
    }
};

// SET_CHAR_ONLY_DAMAGED_BY_PLAYER and SET_CAR_ONLY_DAMAGED_BY_PLAYER (a mission's target, a vigilante's criminals)
// let through only FindPlayerPed's hits (CEventDamage::AffectsPed, CVehicle::CanVehicleBeDamaged): the other
// player is a player too, so the flag is off while its hit is applied
struct AsPlayersHit {
    uint32_t& flags;
    bool had;
    explicit AsPlayersHit(CPhysical* e) : flags(e->PhysicalFlags()), had(flags & CPhysical::PF_INVULNERABLE) {
        flags &= ~CPhysical::PF_INVULNERABLE;
    }
    ~AsPlayersHit() {
        if (had) flags |= CPhysical::PF_INVULNERABLE;
    }
};

bool IsCollision(int weapon) { return weapon == WEAPON_RAMMEDBYCAR || weapon == WEAPON_RUNOVERBYCAR; }
// a bullet or a blow: the shooter's game sees where it hits
bool IsDirect(int weapon) {
    return weapon <= WEAPON_LAST_MELEE || (weapon >= WEAPON_PISTOL && weapon <= WEAPON_SNIPERRIFLE) || weapon == WEAPON_MINIGUN ||
           weapon == WEAPON_UZI_DRIVEBY;
}
// a spray never reaches a mirror (mirrors are fire-proof and CShotInfo skips them): the owner applies the remote
// player's own spray on its peds
bool IsSpray(int weapon) { return weapon == WEAPON_SPRAYCAN || weapon == WEAPON_EXTINGUISHER; }

CPed* DriverOf(CEntity* e) { return e && e->IsVehicle() ? ((CVehicle*)e)->Driver() : nullptr; }

bool IsLocalPlayerEntity(CEntity* e) {
    CPlayerPed* me = FindPlayerPed(0);
    return e && me && (e == me || DriverOf(e) == me);
}

bool IsRemotePlayerEntity(CEntity* e) {
    if (!e) return false;
    CPed* ped = e->IsPed() ? (CPed*)e : DriverOf(e);
    return ped && Players::Instance().FindByPed(ped);
}

// the other player, its car, or a car or ped its game owns (a copy here)
bool FromOtherSide(CEntity* e) {
    if (!e) return false;
    if (IsRemotePlayerEntity(e)) return true;
    if (e->IsVehicle()) {
        NetVehicle* nv = Vehicles::Instance().Find((CVehicle*)e);
        return nv && Vehicles::Instance().IsMirror(*nv);
    }
    NetPed* np = e->IsPed() ? Peds::Instance().Find((CPed*)e) : nullptr;
    return np && np->owner != Net().MyId();
}

void SendDamage(uint8_t kind, uint8_t owner, uint16_t netId, int weapon, float damage, int piece, int dir, const CVector& pos) {
    Writer w(proto::G_DAMAGE);
    w.U8(kind);
    w.U8(owner);
    w.U16(netId);
    w.U8((uint8_t)weapon);
    w.F32(damage);
    w.U8((uint8_t)piece);
    w.U8((uint8_t)dir);
    w.Vec({pos.x, pos.y, pos.z});
    Net().SendTo(owner, proto::CH_RELIABLE, w);  // the game it is for decides, the others need not hear of it
}

using GenerateDamageFn = bool(__cdecl*)(CPed*, CEntity*, int, int, int, int);
GenerateDamageFn g_origGenerateDamage = nullptr;

// CWeapon::GenerateDamageEvent(victim, creator, weapon, damage, bodyPart, direction): every ped damage
// (bullets, melee, explosions, fire, vehicle hits) goes through it. It tells whether the hit counts (blood, a fight
// hit): one decided in the other game still does here.
bool __cdecl HookGenerateDamageEvent(CPed* victim, CEntity* creator, int weapon, int damage, int piece, int dir) {
    if (Net().Online() && victim && core::IsPlayerReady()) {
        if (cam::WideScreenOn() && FromOtherSide(creator)) return true;  // see the top of the file
        bool fromRemote = IsRemotePlayerEntity(creator);
        if (fromRemote && victim == FindPlayerPed(0))  // see the top of the file
            return Net().FriendlyFire() && !IsDirect(weapon) ? g_origGenerateDamage(victim, creator, weapon, damage, piece, dir) : true;
        if (fromRemote && weapon == WEAPON_EXPLOSION) {  // see the top of the file
            NetPed* np = Peds::Instance().Find(victim);
            if ((np && np->owner != Net().MyId()) || Players::Instance().FindByPed(victim)) return true;
            KeepWanted keep;
            return g_origGenerateDamage(victim, creator, weapon, damage, piece, dir);
        }
        if (!IsCollision(weapon) && fromRemote && !IsSpray(weapon)) return true;
        if (RemotePlayer* rp = Players::Instance().FindByPed(victim)) {
            if (weapon <= WEAPON_LAST_MELEE && creator && creator->IsPed() && !IsLocalPlayerEntity(creator)) {
                NetPed* attacker = Peds::Instance().Find((CPed*)creator);
                SendDamage(TARGET_PLAYER, rp->id, attacker ? attacker->netId : 0, weapon, (float)damage, piece, dir,
                           victim->GetPosition());
                LOGD("NPC %04X hit player %d: weapon %d, damage %d", attacker ? attacker->netId : 0, rp->id, weapon, damage);
                return true;
            }
            if (Net().FriendlyFire() && IsDirect(weapon) && IsLocalPlayerEntity(creator)) {
                SendDamage(TARGET_PLAYER_BY_PLAYER, rp->id, 0, weapon, (float)damage, piece, dir, victim->GetPosition());
                LOGI("Hit player %d: weapon %d, damage %d", rp->id, weapon, damage);
                return true;
            }
        }
        if (NetPed* np = Peds::Instance().Find(victim); np && np->owner != Net().MyId()) {
            if (!IsCollision(weapon) && IsLocalPlayerEntity(creator) && !np->deadApplied) {
                SendDamage(TARGET_PED, np->owner, np->netId, weapon, (float)damage, piece, dir, victim->GetPosition());
                np->lastHitMs = NowMs();
                // the crime is ours (the owner applies the hit without charging anybody): CCrime::ReportCrime
                Call<0x532010>(np->pedType == PED_TYPE_COP ? CRIME_DAMAGED_COP : CRIME_DAMAGED_PED, (CEntity*)victim, (CPed*)FindPlayerPed(0));
                LOGI("Hit ped %04X of player %d: weapon %d, damage %d", np->netId, np->owner, weapon, damage);
            }
            return true;
        }
    }
    return g_origGenerateDamage(victim, creator, weapon, damage, piece, dir);
}

// CPed::KillPedWithCar(car, impulse, ...): a car knocks a ped down. The driver's game decides it: a mirror that the
// local player's car hits is sent to its owner (a mirror is collision-proof and would stand like a stone), and the
// remote player's car here hits nobody by itself, but this game's player with friendly fire.
using KillWithCarFn = void(__fastcall*)(CPed*, void*, CVehicle*, float, int);
KillWithCarFn g_origKillWithCar = nullptr;
void __fastcall HookKillPedWithCar(CPed* ped, void* edx, CVehicle* car, float impulse, int flag) {
    if (Net().Online() && car && core::IsPlayerReady()) {
        if (cam::WideScreenOn() && FromOtherSide(car)) return;
        CPed* driver = car->Driver();
        if (driver && Players::Instance().FindByPed(driver)) {
            if (Net().FriendlyFire() && ped == FindPlayerPed(0)) g_origKillWithCar(ped, edx, car, impulse, flag);
            return;
        }
        if (NetPed* np = Peds::Instance().Find(ped); np && np->owner != Net().MyId()) {
            if (driver == FindPlayerPed(0) && !np->deadApplied && NowMs() - np->lastRamMs > 500) {
                np->lastRamMs = np->lastHitMs = NowMs();
                SendDamage(TARGET_PED, np->owner, np->netId, WEAPON_RAMMEDBYCAR, std::clamp(impulse, 1.0f, 5000.0f), 0, 0,
                           ped->GetPosition());
                Call<0x532010>(np->pedType == PED_TYPE_COP ? CRIME_DAMAGED_COP : CRIME_DAMAGED_PED, (CEntity*)ped, driver);
                LOGI("Rammed ped %04X of player %d (impulse %.0f)", np->netId, np->owner, impulse);
            }
            return;
        }
    }
    g_origKillWithCar(ped, edx, car, impulse, flag);
}

using InflictDamageFn = void(__fastcall*)(CVehicle*, void*, CEntity*, int, float, CVector);
InflictDamageFn g_origInflictDamage = nullptr;

// CVehicle::InflictDamage(damager, weapon, damage, position): bullets, explosions and fire on vehicles
void __fastcall HookInflictDamage(CVehicle* v, void* edx, CEntity* damager, int weapon, float damage, CVector pos) {
    if (Net().Online() && v && core::IsPlayerReady()) {
        if (!IsCollision(weapon) && IsRemotePlayerEntity(damager)) return;
        if (cam::WideScreenOn() && FromOtherSide(damager)) return;
        if (NetVehicle* nv = Vehicles::Instance().Find(v); nv && Vehicles::Instance().IsMirror(*nv)) {
            if (!IsCollision(weapon) && IsLocalPlayerEntity(damager))
                SendDamage(TARGET_VEHICLE, nv->owner, nv->netId, weapon, damage, 0, 0, pos);
            return;
        }
    }
    g_origInflictDamage(v, edx, damager, weapon, damage, pos);
}

using RegisterKillFn = void(__cdecl*)(CPed*, CEntity*, int, bool);
RegisterKillFn g_origRegisterKill = nullptr;

// CEventHandler::RegisterKill(ped, killer, weapon, headshot): a ped died of a hit. This game's ped killed by the other
// player is credited to that player's game and counts here as killed by somebody else (see the top of the file).
void __cdecl HookRegisterKill(CPed* ped, CEntity* killer, int weapon, bool headshot) {
    CPlayerPed* me = FindPlayerPed(0);
    if (Net().Online() && ped && killer && killer->IsPed() && core::IsPlayerReady()) {
        if (RemotePlayer* rp = Players::Instance().FindByPed((CPed*)killer)) {
            if (NetPed* np = Peds::Instance().Find(ped); np && np->owner == Net().MyId())
                SendDamage(KILLED_BY_YOU, rp->id, np->netId, weapon, 1.0f, headshot, 0, ped->GetPosition());
            killer = nullptr;
        } else if (killer == me && !Net().IsHost() && !me->InVehicle() &&
                   (ped->PedType() == PED_TYPE_GANG1 || ped->PedType() == PED_TYPE_GANG3)) {
            SendDamage(GANG_KILL, Net().HostId(), 0, ped->PedType(), 1.0f, 0, 0, ped->GetPosition());
        }
    }
    g_origRegisterKill(ped, killer, weapon, headshot);
}

void HandleDamage(uint8_t from, Reader& r) {
    uint8_t kind = r.U8(), owner = r.U8();
    uint16_t netId = r.U16();
    int weapon = r.U8();
    float damage = r.F32();
    int piece = r.U8(), dir = r.U8();
    Vec3 p = r.Vec();
    if (!r.Ok() || owner != Net().MyId() || !core::IsPlayerReady() || damage <= 0.0f || damage > 10000.0f) return;
    // the weapon info (80 entries), the ped pieces (to the head, 9) and the four hit directions are read at these unchecked
    if (weapon > 58 /* WEAPON_FLARE */ || piece > 9 || dir > 3) return;
    if (cam::WideScreenOn()) return;  // a scripted scene: the other game's hits are dropped (see the top of the file)
    const RemotePlayer* rp = Players::Instance().Get(from);
    CEntity* attacker = rp ? rp->ped : nullptr;
    if (kind == TARGET_PED) {
        if (CPed* ped = Peds::Instance().FindOwned(netId)) {
            {
                KeepWanted keep;
                AsPlayersHit player(ped);
                CVehicle* car = rp && rp->ped && rp->ped->InVehicle() ? rp->ped->Vehicle() : nullptr;
                if (weapon == WEAPON_RAMMEDBYCAR && car) g_origKillWithCar(ped, nullptr, car, damage, 0);
                else if (weapon != WEAPON_RAMMEDBYCAR) g_origGenerateDamage(ped, attacker, weapon, (int)damage, piece, dir);
            }
            LOGI("Player %d hit our ped %04X: weapon %d, damage %.0f, health now %.0f", from, netId, weapon, damage, ped->Health());
            if (ped->Health() <= 0.0f) {  // the loot falls in the killer's game
                Cmd(0x087E).I(GetPedRef(ped)).I(0).Run();  // SET_CHAR_DROPS_WEAPONS_WHEN_DEAD false
                ped->Money() = 0;
            }
        }
    } else if (kind == TARGET_PLAYER_BY_PLAYER) {
        if (!Net().FriendlyFire()) return;
        {
            KeepWanted keep;
            g_origGenerateDamage(FindPlayerPed(0), attacker, weapon, (int)damage, piece, dir);
        }
        LOGI("Player %d hit us: weapon %d, damage %.0f, health now %.0f", from, weapon, damage, FindPlayerPed(0)->Health());
    } else if (kind == TARGET_PLAYER) {
        int h = Peds::Instance().HandleOf(netId);
        g_origGenerateDamage(FindPlayerPed(0), h >= 0 ? GetPed(h) : nullptr, weapon, (int)damage, piece, dir);
        LOGI("An NPC %04X of player %d hit us: weapon %d, damage %.0f", netId, from, weapon, damage);
    } else if (kind == TARGET_VEHICLE) {
        NetVehicle* nv = Vehicles::Instance().Find(netId);
        if (nv && !Vehicles::Instance().IsMirror(*nv) && IsVehicleValid(nv->veh)) {
            KeepWanted keep;
            AsPlayersHit player(nv->veh);
            g_origInflictDamage(nv->veh, nullptr, attacker, weapon, damage, CVector{p.x, p.y, p.z});
        }
    } else if (kind == KILLED_BY_YOU) {
        int h = Peds::Instance().HandleOf(netId);  // its copy here: a civilian, it counts as what its owner has
        if (CPed* ped = h >= 0 ? GetPed(h) : nullptr) {
            NetPed* np = Peds::Instance().Find(ped);
            int32_t type = ped->PedType();
            if (np) ped->PedType() = np->pedType;
            HookRegisterKill(ped, FindPlayerPed(0), weapon, piece != 0);
            ped->PedType() = type;
        }
        LOGI("We killed ped %04X of player %d: weapon %d", netId, from, weapon);
    } else if (kind == GANG_KILL && Net().IsHost()) {
        const CVector& me = FindPlayerPed(0)->GetPosition();
        if ((weapon == PED_TYPE_GANG1 || weapon == PED_TYPE_GANG3) && std::hypot(me.x - p.x, me.y - p.y) < 100.0f)
            Call<0x443950>(weapon);  // CGangWars::AddKillToProvocation
    }
}

class Damage : public Module {
public:
    const char* Name() const override { return "Damage"; }
    void OnInstallHooks() override {
        mem::HookFn(0x73A530, &HookGenerateDamageEvent, &g_origGenerateDamage, "CWeapon::GenerateDamageEvent");
        mem::HookFn(0x6D7C90, &HookInflictDamage, &g_origInflictDamage, "CVehicle::InflictDamage");
        mem::HookFn(0x5F0360, &HookKillPedWithCar, &g_origKillWithCar, "CPed::KillPedWithCar");
        mem::HookFn(0x4B9340, &HookRegisterKill, &g_origRegisterKill, "CEventHandler::RegisterKill");
        Net().SetHandler(proto::G_DAMAGE, &HandleDamage);
    }
};

Damage g_damage;
struct Reg {
    Reg() { core::RegisterModule(&g_damage); }
} g_reg;
}  // namespace

}  // namespace coop
