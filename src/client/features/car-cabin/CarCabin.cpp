// Inside a car with other players.
//
// The radio is the driver's: a player driving tells the station it plays (G_RADIO) and every player riding with it
// tunes to the same one; a passenger of a player's car cannot change it (CPad::NextStationJustUp / LastStationJustUp
// answer no for it: the keys, the pad's d-pad and the mouse wheel all come through them). With an NPC or nobody at the
// wheel the passenger tunes as it likes.
//
// A passenger's mouse wheel picks the gun to lean out of the window with (the guns a drive-by can fire: pistols,
// shotguns, SMGs, rifles with ammunition), as the wheel does on foot; a passenger's wheel never retunes the radio. Leaning out at that moment, the passenger draws
// back in and leans out again with the new gun (while the aim button is held).
#include <map>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
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

constexpr uintptr_t kMouseState = 0xB73418;  // CPad::NewMouseControllerState: +3 wheel up, +4 wheel down (this frame)
constexpr int TASK_SIMPLE_GANG_DRIVEBY = 1022;

std::map<uint16_t, int8_t> g_stations;  // the station each player-driven car plays (by its net id)
int8_t g_sentStation = -100;
uint16_t g_sentCar = 0;
uint64_t g_sentMs = 0;
// the driver: what the radio says now and since when (a retune passes through no station: only a station held a
// moment is sent, or the passengers chased each step and heard nothing but the retune's noise)
int8_t g_seenStation = -100;
uint64_t g_seenMs = 0;
// a passenger: the station asked for last, in which car (asked once per change, never by comparing with what the radio
// says: during a retune it says something else, and asking again restarted the retune for ever)
int8_t g_appliedStation = -100;
uint16_t g_appliedCar = 0;

int8_t CurrentStation() {
    int32_t s = -1;
    Cmd(0x051E).Out(&s).Run();  // GET_RADIO_CHANNEL
    return (int8_t)s;
}

// the car this game's player rides in as a passenger while a player drives it
CVehicle* PlayersCar(CPlayerPed* me) {
    CVehicle* v = me ? me->VehicleIfIn() : nullptr;
    if (!v || v->Driver() == me || !v->Driver()) return nullptr;
    return Players::Instance().FindByPed(v->Driver()) ? v : nullptr;
}

bool WheelTurned() { return Ref<uint8_t>(kMouseState + 3) || Ref<uint8_t>(kMouseState + 4); }

// a passenger's radio: the driver's when a player drives; the mouse wheel is a passenger's gun choice in any car
bool RadioLocked() {
    if (!Net().Online() || !core::IsPlayerReady()) return false;
    CPlayerPed* me = FindPlayerPed(0);
    if (PlayersCar(me)) return true;
    CVehicle* v = me ? me->VehicleIfIn() : nullptr;
    return v && v->Driver() != me && WheelTurned();
}

using StationKeyFn = bool(__fastcall*)(void*, void*);
StationKeyFn g_origNext = nullptr, g_origLast = nullptr;
bool __fastcall HookNextStation(void* pad, void* edx) { return !RadioLocked() && g_origNext(pad, edx); }
bool __fastcall HookLastStation(void* pad, void* edx) { return !RadioLocked() && g_origLast(pad, edx); }

void HandleRadio(uint8_t from, Reader& r) {
    uint16_t car = r.U16();
    int8_t station = r.I8();
    if (!r.Ok()) return;
    g_stations[car] = station;
}

// the driver: its station to the others
void SendStation(CPlayerPed* me) {
    CVehicle* v = me->VehicleIfIn();
    if (!v || v->Driver() != me || v->IsTrain()) return;
    NetVehicle* nv = Vehicles::Instance().Find(v);
    if (!nv) return;
    int8_t s = CurrentStation();
    uint64_t now = NowMs();
    if (s != g_seenStation) g_seenStation = s, g_seenMs = now;
    if (s < 0 || now - g_seenMs < 800) return;  // retuning
    if (s == g_sentStation && nv->netId == g_sentCar && now - g_sentMs < 3000) return;
    if (s != g_sentStation || nv->netId != g_sentCar) LOGD("Radio: station %d in car %u", s, nv->netId);
    g_sentStation = s, g_sentCar = nv->netId, g_sentMs = now;
    Writer w(proto::G_RADIO);
    w.U16(nv->netId);
    w.I8(s);
    Net().Send(proto::CH_RELIABLE, w);
}

// a passenger: the driver's station
void FollowStation(CPlayerPed* me) {
    CVehicle* v = PlayersCar(me);
    NetVehicle* nv = v ? Vehicles::Instance().Find(v) : nullptr;
    if (!nv) {
        g_appliedCar = 0;
        return;
    }
    auto it = g_stations.find(nv->netId);
    if (it == g_stations.end() || (g_appliedCar == nv->netId && g_appliedStation == it->second)) return;
    g_appliedCar = nv->netId;
    g_appliedStation = it->second;
    if (it->second == CurrentStation()) return;  // already there
    Cmd(0x041E).I(it->second).Run();  // SET_RADIO_CHANNEL
    LOGI("Radio: tuned to the driver's station %d", it->second);
}

bool DriveByGun(int type, int ammo) {
    if (type < 22 || type > 33 || ammo <= 0) return false;  // pistols .. the country rifle (the sniper rifle aims in 1st person)
    void* info = Call<0x743C60, void*>(type, (uint8_t)1);   // CWeaponInfo::GetWeaponInfo
    return info && Field<int32_t>(info, 0) == 1;            // m_nWeaponFire: FIRE_INSTANT_HIT
}

// a passenger's wheel: the next gun it has for a drive-by
void WheelGun(CPlayerPed* me) {
    CVehicle* v = me->VehicleIfIn();
    if (!v || v->Driver() == me || v->IsTrain() || GetPad(0)->DisablePlayerControls) return;
    int dir = Ref<uint8_t>(kMouseState + 3) ? -1 : Ref<uint8_t>(kMouseState + 4) ? 1 : 0;
    if (!dir) return;
    int cur = me->ActiveWeaponSlot();
    for (int step = 1; step < NUM_WEAPON_SLOTS; ++step) {
        int slot = (cur + dir * step + NUM_WEAPON_SLOTS * 2) % NUM_WEAPON_SLOTS;
        const CWeapon& w = me->Weapons()[slot];
        if (!DriveByGun(w.type, w.ammoTotal)) continue;
        // CPed::SetCurrentWeapon(slot). Not SET_CURRENT_CHAR_WEAPON: for a player it only notes the choice
        // (CPlayerPedData::m_nChosenWeapon) for the weapon switch, which a player in a car never runs
        // (m_bInVehicleDontAllowWeaponChange)
        CallMethod<0x5E61F0>(me, slot);
        // leaning out with the old one: back in (CTaskSimpleGangDriveBy's way out, command 7), out again with the new
        void* intel = me->Intelligence();
        if (void* task = intel ? CallMethod<0x681740, void*>((uint8_t*)intel + 4, TASK_SIMPLE_GANG_DRIVEBY) : nullptr)
            Field<int8_t>(task, 0x0F) = 7;
        LOGD("Passenger's gun: %d", w.type);
        return;
    }
}

class CarCabin : public Module {
public:
    const char* Name() const override { return "CarCabin"; }
    void OnInstallHooks() override {
        mem::HookFn(0x5405B0, &HookNextStation, &g_origNext, "CPad::NextStationJustUp");
        mem::HookFn(0x5405E0, &HookLastStation, &g_origLast, "CPad::LastStationJustUp");
        Net().SetHandler(proto::G_RADIO, &HandleRadio);
    }
    void OnSessionEnd() override { g_stations.clear(); }
    void OnPostProcess() override {
        if (!Net().Online() || !core::IsPlayerReady()) return;
        CPlayerPed* me = FindPlayerPed(0);
        if (!me) return;
        SendStation(me);
        FollowStation(me);
        WheelGun(me);
    }
};

CarCabin g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

}  // namespace coop
