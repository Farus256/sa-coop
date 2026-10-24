// The safe houses' garages, which keep the cars left in them, belong to the host's world. What they keep is part of
// the host's game and its save, and the partner's game loads that world again and again (story progress), so the
// partner's game neither puts cars away nor brings any out: both games doing it made every kept car twice, and a car
// the partner had put away was gone with the next world. The partner's game asks the host's instead. When a garage
// closes behind a car of the partner's, the host's game keeps it (as it would its own) and the partner's lets it go.
// When a garage opens for the partner, the host's game hands over what it keeps there and the partner's brings the
// cars out (where the partner is, the ground is loaded). The host's snapshots of its world for the partners are saves too: they no longer slam an open garage
// shut on the cars in it.
// The doors of the shops (Pay 'n' Spray, bomb shops, mod shops) move in both games as in the game of the player who
// uses the garage (see HookGarageUpdate).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/player/Players.h"
#include "entities/session/Flow.h"
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

constexpr uintptr_t kGarages = 0x96C048, kNumGarages = 0x96C024;  // CGarages::aGarages (CGarage 0xD8 each), NumGarages
constexpr uintptr_t kStoredCars = 0x96ABD8;                         // CGarages::aCarsInSafeHouse: 4 CStoredCar (0x40) a house
constexpr size_t kGarageSize = 0xD8;
enum : uint8_t { GARAGE_STORE = 1, GARAGE_KEPT = 2, GARAGE_BRING_OUT = 3, GARAGE_CARS = 4, GARAGE_BACK = 5 };
constexpr size_t kStoredCarSize = 0x40;

uint8_t TypeOf(void* g) { return Field<uint8_t>(g, 0x4C); }  // CGarage::m_nType
bool IsHideout(uint8_t t) { return (t >= 16 && t <= 18) || (t >= 24 && t <= 32) || (t >= 39 && t <= 42) || t == 45; }
void* GarageAt(int i) {
    return i >= 0 && i < 50 && i < Ref<int32_t>(kNumGarages) ? (void*)(kGarages + (uintptr_t)i * kGarageSize) : nullptr;
}
uint8_t IndexOf(void* g) { return (uint8_t)(((uintptr_t)g - kGarages) / kGarageSize); }
std::string NameOf(void* g) { return std::string(Field<char[8]>(g, 0x44), strnlen(Field<char[8]>(g, 0x44), 8)); }
uint8_t* StoreOf(void* g) {  // CGarages::FindSafeHouseIndexForGarageType
    return (uint8_t*)(kStoredCars + (uintptr_t)Call<0x4489F0, int>((int)TypeOf(g)) * 0x100);
}
bool Inside(void* g, const CVector& p) { return CallMethod<0x448740, bool>(g, p); }  // CGarage::IsPointInsideGarage

// the partner's game in the host's world: its garages keep nothing of their own
bool Partner() { return Net().Online() && !Net().IsHost() && flow::InHostWorld(); }

bool Occupied(CVehicle* v) {
    if (v->Driver()) return true;
    for (int i = 0; i < 8; ++i)
        if (v->Passengers()[i]) return true;
    return false;
}

// what CGarage::StoreAndRemoveCarsForThisHideOut would put away of ours goes to the host's store; our cars stay here
// until the host has them
void AskToKeep(void* garage) {
    if (!core::IsPlayerReady()) return;  // a world loading: nothing of ours in it
    CPoolHeader* pool = VehiclePool();
    std::vector<uint16_t> ids;
    for (int i = 0; pool && i < pool->size && ids.size() < 4; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* v = (CVehicle*)(pool->objects + (size_t)i * kVehiclePoolElemSize);
        NetVehicle* nv = Vehicles::Instance().Find(v);
        if (nv && !Vehicles::Instance().IsMirror(*nv) && !Occupied(v) && Inside(garage, v->GetPosition())) ids.push_back(nv->netId);
    }
    if (ids.empty()) return;
    Writer w(proto::G_GARAGE);
    w.U8(GARAGE_STORE);
    w.U8(IndexOf(garage));
    w.U8((uint8_t)ids.size());
    for (uint16_t id : ids) w.U16(id);
    Net().SendTo(Net().HostId(), proto::CH_RELIABLE, w);
    LOGI("Garage %s: %zu car(s) of ours go to the host's store", NameOf(garage).c_str(), ids.size());
}

using StoreFn = void(__fastcall*)(void*, void*, void*, int);
StoreFn g_origStoreClosing = nullptr, g_origStoreBeforeSave = nullptr;
void __fastcall HookStoreClosing(void* garage, void* edx, void* stored, int maxSlot) {  // the door closed (CGarage::Update)
    if (!Partner()) return g_origStoreClosing(garage, edx, stored, maxSlot);
    AskToKeep(garage);
}
void __fastcall HookStoreBeforeSave(void* garage, void* edx, void* stored, int maxSlot) {  // CloseHideOutGaragesBeforeSave
    if (!Partner()) return g_origStoreBeforeSave(garage, edx, stored, maxSlot);
    AskToKeep(garage);
}

using RestoreFn = bool(__fastcall*)(void*, void*, void*);
// the game's own until redirected: a site another mod changed is left alone, and this is still called
RestoreFn g_origRestore = (RestoreFn)0x448550;
bool __fastcall HookRestore(void* garage, void* edx, void* stored) {  // CGarage::RestoreCarsForThisHideOut
    if (!Partner()) return g_origRestore(garage, edx, stored);
    if (core::IsPlayerReady()) {
        Writer w(proto::G_GARAGE);
        w.U8(GARAGE_BRING_OUT);
        w.U8(IndexOf(garage));
        Net().SendTo(Net().HostId(), proto::CH_RELIABLE, w);
    }
    return true;  // nothing to wait for here: the door opens (CGarage::Update asks until all cars are out)
}

using CloseFn = void(__cdecl*)();
CloseFn g_origCloseBeforeSave = nullptr;
void __cdecl HookCloseBeforeSave() {  // CGarages::Save
    if (core::SavingSnapshot()) return;  // the partners' copy of the world: an open garage stays open with its cars
    g_origCloseBeforeSave();
}

// What CStoredCar::RestoreCar (0x447E40) can make here: it asks the streamer for the model and every part unchecked (a
// model of a mod this game lacks crashes it), and CVehicle::SetRemap reads the model's four paintjobs at the index.
bool Restorable(const uint8_t* car) {
    if (!IsVehicleModel(Ref<uint16_t>((uintptr_t)car + 0x12))) return false;  // m_wModelIndex
    for (int i = 0; i < 15; ++i) {  // m_awCarMods, -1 for none
        int part = Ref<int16_t>((uintptr_t)car + 0x14 + (uintptr_t)i * 2);
        if (part != -1 && !IsValidModel(part)) return false;
    }
    int8_t paintjob = Ref<int8_t>((uintptr_t)car + 0x3A);
    return paintjob >= -1 && paintjob < 4;
}

void Handle(uint8_t from, Reader& r) {
    uint8_t op = r.U8(), index = r.U8();
    void* g = GarageAt(index);
    if (!r.Ok() || from == Net().MyId() || !g || !IsHideout(TypeOf(g)) || !core::IsPlayerReady()) return;
    if (op == GARAGE_STORE && Net().IsHost()) {
        uint8_t n = r.U8();
        uint8_t* store = StoreOf(g);
        std::vector<uint16_t> kept;
        for (int i = 0; i < n && i < 4; ++i) {
            uint16_t id = r.U16();
            NetVehicle* nv = Vehicles::Instance().Find(id);
            if (!r.Ok() || !nv || nv->owner != from || !nv->veh || !IsVehicleValid(nv->veh) || Occupied(nv->veh) ||
                !Inside(g, nv->veh->GetPosition()))
                continue;
            uint8_t* slot = nullptr;
            for (int k = 0; k < 4 && !slot; ++k)
                if (!Ref<uint16_t>((uintptr_t)store + (uintptr_t)k * 0x40 + 0x12)) slot = store + k * 0x40;  // CStoredCar::m_wModelIndex
            if (!slot) break;  // all four places taken: the car stays where it is
            CallMethod<0x449760>(slot, nv->veh);  // CStoredCar::StoreCar
            kept.push_back(id);
        }
        if (kept.empty()) return;
        Writer w(proto::G_GARAGE);
        w.U8(GARAGE_KEPT);
        w.U8(index);
        w.U8((uint8_t)kept.size());
        for (uint16_t id : kept) w.U16(id);
        Net().SendTo(from, proto::CH_RELIABLE, w);
        LOGI("Garage %s keeps %zu car(s) of player %d", NameOf(g).c_str(), kept.size(), from);
    } else if (op == GARAGE_KEPT && from == Net().HostId() && !Net().IsHost()) {
        uint8_t n = r.U8();
        int gone = 0;
        for (int i = 0; i < n && i < 4; ++i) {
            NetVehicle* nv = Vehicles::Instance().Find(r.U16());
            if (!r.Ok() || !nv || Vehicles::Instance().IsMirror(*nv) || !nv->veh || !IsVehicleValid(nv->veh) || Occupied(nv->veh)) continue;
            StopEntering(nv->veh);
            Cmd(0x00A6).I(GetVehicleRef(nv->veh)).Run();  // DELETE_CAR: the vehicle sync tells the host's game it is gone
            ++gone;
        }
        LOGI("Garage %s: the host keeps %d car(s) of ours now", NameOf(g).c_str(), gone);
    } else if (op == GARAGE_BRING_OUT && Net().IsHost()) {  // the cars it keeps go to that player's game alone: out of our store
        uint8_t* store = StoreOf(g);
        Writer w(proto::G_GARAGE);
        w.U8(GARAGE_CARS);
        w.U8(index);
        std::vector<uint8_t*> cars;
        for (int k = 0; k < 4; ++k)
            if (Ref<uint16_t>((uintptr_t)store + (uintptr_t)k * kStoredCarSize + 0x12)) cars.push_back(store + k * kStoredCarSize);
        if (cars.empty()) return;
        w.U8((uint8_t)cars.size());
        for (uint8_t* c : cars) {
            w.Bytes(c, kStoredCarSize);
            Ref<uint16_t>((uintptr_t)c + 0x12) = 0;
        }
        Net().SendTo(from, proto::CH_RELIABLE, w);
        LOGI("Garage %s opened for player %d: its %zu car(s) go out there", NameOf(g).c_str(), from, cars.size());
    } else if (op == GARAGE_CARS && from == Net().HostId() && !Net().IsHost()) {
        uint8_t n = r.U8(), cars[4 * kStoredCarSize] = {};
        for (int k = 0; k < n && k < 4; ++k) {
            std::vector<uint8_t> c = r.Bytes(kStoredCarSize);
            if (c.size() == kStoredCarSize) memcpy(cars + k * kStoredCarSize, c.data(), kStoredCarSize);
        }
        if (!r.Ok()) return;
        uint8_t kept[4 * kStoredCarSize] = {};  // cars this game cannot make: they stay the host's as they are
        for (int k = 0; k < 4; ++k) {
            uint8_t* c = cars + k * kStoredCarSize;
            int model = Ref<uint16_t>((uintptr_t)c + 0x12);
            if (!model) continue;
            // CStoredCar::RestoreCar makes nothing of a model not in memory yet (it only asks for it)
            if (Restorable(c)) LoadModelNow(model);
            else std::swap_ranges(c, c + kStoredCarSize, kept + k * kStoredCarSize);
        }
        g_origRestore(g, nullptr, cars);  // CGarage::RestoreCarsForThisHideOut on the host's records: the cars are ours
        // what could not come out goes back into the host's store
        Writer w(proto::G_GARAGE);
        w.U8(GARAGE_BACK);
        w.U8(index);
        std::vector<uint8_t*> left;
        for (int k = 0; k < 4; ++k) {
            if (Ref<uint16_t>((uintptr_t)cars + (uintptr_t)k * kStoredCarSize + 0x12)) left.push_back(cars + k * kStoredCarSize);
            if (Ref<uint16_t>((uintptr_t)kept + (uintptr_t)k * kStoredCarSize + 0x12)) left.push_back(kept + k * kStoredCarSize);
        }
        if (!left.empty()) {
            w.U8((uint8_t)left.size());
            for (uint8_t* c : left) w.Bytes(c, kStoredCarSize);
            Net().SendTo(Net().HostId(), proto::CH_RELIABLE, w);
        }
        LOGI("Garage %s: %d car(s) of the host's store out here, %zu back", NameOf(g).c_str(), n - (int)left.size(), left.size());
    } else if (op == GARAGE_BACK && Net().IsHost()) {
        uint8_t n = r.U8(), *store = StoreOf(g);
        for (int k = 0; k < n && k < 4; ++k) {
            std::vector<uint8_t> c = r.Bytes(kStoredCarSize);
            uint8_t* slot = nullptr;
            for (int s = 0; s < 4 && !slot; ++s)
                if (!Ref<uint16_t>((uintptr_t)store + (uintptr_t)s * kStoredCarSize + 0x12)) slot = store + s * kStoredCarSize;
            if (r.Ok() && c.size() == kStoredCarSize && slot) memcpy(slot, c.data(), kStoredCarSize);
        }
        LOGI("Garage %s: %d car(s) back in the store", NameOf(g).c_str(), (int)n);
    }
}

// ---------------------------------------------------------------- doors of the shops
// CGarage::Update moves a shop's door for this game's player only: the other game's Pay 'n' Spray stood open while the
// player inside had it shut, and a mod shop stayed shut in front of the other player's car. The game whose player uses
// a garage drives its door in every game; the others slide their door as a script's garage (type 19) does, with the
// door's sound and collision but no respray, bomb, money, wanted level or frozen controls there.
constexpr int kMaxGarages = 50;
enum : uint8_t { DOOR_SHUT = 0, DOOR_OPEN = 1, DOOR_NONE = 2 };  // NONE: the sender's door follows another game's
enum : uint8_t { STATE_CLOSED = 0, STATE_OPEN = 1, STATE_CLOSING = 2, STATE_OPEN_WAITING = 4, STATE_OPENING = 3 };
struct Door {
    uint8_t remote[proto::kMaxPlayers];  // each other game's door
    uint8_t sent = 0xFF;                 // ours, as last sent
    bool mirroring = false;              // ours follows another game's
    Door() { memset(remote, DOOR_NONE, sizeof remote); }
};
Door g_doors[kMaxGarages];

// The doors a script opens and shuts (mission garages, the burglary lockup BURG_LK, the script-controlled ones): during
// the host's story mission they are its mission's, and the partner's followed nothing. "Home Invasion" opens the
// lockup for the van: shut in the partner's game, the van's copy was driven into it and the partner fell through.
bool ScriptDoor(uint8_t t) { return t == 1 || t == 14 || t == 15 || (t >= 19 && t <= 21) || t == 23; }
bool DoorShared(uint8_t t) { return (t >= 2 && t <= 5) || (t >= 36 && t <= 38) || ScriptDoor(t); }  // + bomb shops, Pay 'n' Spray, mod shops

// Whose game drives the door: the player's whose car is in the garage; else of the players within 30 m (a mod shop
// keeps its door open that near) one in a car before one on foot, the nearer before the farther (in steps of 5 m),
// the host before the others, a lower number before a higher one; 255: nobody near, each game its own. Every game
// comes to the same answer from the players' places. A script's door during the host's mission: the host's game
// drives it in every game; out of a mission each game's own scripts do.
uint8_t Controller(void* g) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me) return 255;
    if (ScriptDoor(TypeOf(g))) return HostMissionRecent(0) ? Net().HostId() : 255;
    CVector c{(Field<float>(g, 0x28) + Field<float>(g, 0x2C)) / 2, (Field<float>(g, 0x30) + Field<float>(g, 0x34)) / 2,
              Field<float>(g, 0x08)};  // the middle of its box (m_fLeftCoord..m_fBackCoord), on the floor
    struct Claim {
        uint8_t id;
        bool inside, rides;
        int step;
    };
    std::vector<Claim> claims;
    auto consider = [&](uint8_t id, bool rides, bool outdoors, const CVector& pos, const CVector* carPos) {
        if (!outdoors) return;  // the garages are outdoors
        bool inside = rides && carPos && Inside(g, *carPos);
        float d = (pos - c).Length();
        if (inside || d <= 30.0f) claims.push_back({id, inside, rides, (int)(d / 5.0f)});
    };
    CVehicle* car = FindPlayerVehicle();
    CVector carPos = car ? car->GetPosition() : CVector{};
    consider(Net().MyId(), car != nullptr, me->AreaCode() == 0, me->GetPosition(), car ? &carPos : nullptr);
    for (auto& [id, rp] : Players::Instance().All())
        if (rp.hasState && Net().Peer(id))
            consider(id, rp.mode != RemotePlayer::Mode::OnFoot, rp.state.area == 0, rp.state.pos, &rp.state.pos);
    if (claims.empty()) return 255;
    uint8_t host = Net().HostId();
    auto before = [host](const Claim& a, const Claim& b) {
        if (a.inside != b.inside) return a.inside;
        if (a.rides != b.rides) return a.rides;
        if (a.step != b.step) return a.step < b.step;
        if ((a.id == host) != (b.id == host)) return a.id == host;
        return a.id < b.id;
    };
    return std::min_element(claims.begin(), claims.end(), before)->id;
}

using GarageUpdateFn = void(__fastcall*)(void*, void*, int);
GarageUpdateFn g_origGarageUpdate = nullptr;
void __fastcall HookGarageUpdate(void* g, void* edx, int id) {  // CGarages::Update -> CGarage::Update, every garage
    if (id < 0 || id >= kMaxGarages || !DoorShared(TypeOf(g))) return g_origGarageUpdate(g, edx, id);
    Door& d = g_doors[id];
    uint8_t& state = Field<uint8_t>(g, 0x4D);  // CGarage::m_nDoorState
    bool on = Net().Online() && core::IsPlayerReady();
    uint8_t by = on ? Controller(g) : 255;
    // another game drives it, and that game says it does (it may see the players a moment apart from us)
    uint8_t remote = by < proto::kMaxPlayers && by != Net().MyId() ? d.remote[by] : DOOR_NONE;
    bool theirs = remote != DOOR_NONE;
    if (theirs) {
        if (remote == DOOR_OPEN ? state != STATE_OPEN : state != STATE_CLOSED)
            state = remote == DOOR_OPEN ? STATE_OPENING : STATE_CLOSING;
        uint8_t& type = Field<uint8_t>(g, 0x4C);
        uint8_t own = type;
        type = 19;  // GARAGE_SCRIPT_CONTROLLED: its Update only slides the door
        g_origGarageUpdate(g, edx, id);
        type = own;
    } else {
        // ours again: left shut, a Pay 'n' Spray or a bomb shop would respray or arm our car wherever it is, take our
        // money and clear our wanted level
        if (d.mirroring && TypeOf(g) >= 2 && TypeOf(g) <= 5 && state != STATE_OPEN && state != STATE_OPENING &&
            state != STATE_OPEN_WAITING)
            state = STATE_OPENING;
        g_origGarageUpdate(g, edx, id);
    }
    if (theirs != d.mirroring)
        LOGI("Garage %s: its door %s", NameOf(g).c_str(), theirs ? Format("follows player %d's game", by).c_str() : "is ours again");
    d.mirroring = theirs;
    uint8_t v = theirs ? DOOR_NONE : (state == STATE_OPEN || state == STATE_OPENING || state == STATE_OPEN_WAITING) ? DOOR_OPEN : DOOR_SHUT;
    if (on && v != d.sent) {
        Writer w(proto::G_GARAGE_DOOR);
        w.U8((uint8_t)id);
        w.U8(v);
        Net().Send(proto::CH_RELIABLE, w);
        if (d.sent != 0xFF) LOGD("Garage %s: our door %s", NameOf(g).c_str(), v == DOOR_OPEN ? "open" : v == DOOR_SHUT ? "shut" : "follows");
        d.sent = v;
    }
}

void HandleDoor(uint8_t from, Reader& r) {
    uint8_t id = r.U8(), v = r.U8();
    if (!r.Ok() || from == Net().MyId() || from >= proto::kMaxPlayers || id >= kMaxGarages || v > DOOR_NONE) return;
    if (g_doors[id].remote[from] != v && g_doors[id].mirroring)
        LOGD("Garage %d: player %d's door %s", id, from, v == DOOR_OPEN ? "open" : v == DOOR_SHUT ? "shut" : "follows another");
    g_doors[id].remote[from] = v;
}

class Garages : public Module {
public:
    const char* Name() const override { return "Garages"; }
    void OnInstallHooks() override {
        mem::RedirectCallFn(0x44BE44, &HookStoreClosing, &g_origStoreClosing);        // CGarage::Update
        mem::RedirectCallFn(0x44A1EA, &HookStoreBeforeSave, &g_origStoreBeforeSave);  // CGarages::CloseHideOutGaragesBeforeSave
        mem::RedirectCallFn(0x44C1E1, &HookRestore, &g_origRestore);                  // CGarage::Update
        mem::RedirectCallFn(0x5D3165, &HookCloseBeforeSave, &g_origCloseBeforeSave);  // CGarages::Save
        mem::RedirectCallFn(0x44C910, &HookGarageUpdate, &g_origGarageUpdate);        // CGarages::Update
        Net().SetHandler(proto::G_GARAGE, &Handle);
        Net().SetHandler(proto::G_GARAGE_DOOR, &HandleDoor);
    }
    void OnPeerResync(uint8_t) override {  // its world is new: the next update sends every door again
        for (Door& d : g_doors) d.sent = 0xFF;
    }
    void OnPeerLeave(uint8_t id) override {
        if (id < proto::kMaxPlayers)
            for (Door& d : g_doors) d.remote[id] = DOOR_NONE;
    }
    void OnSessionEnd() override {  // a door that followed another game is handed back by the next update
        for (Door& d : g_doors) {
            memset(d.remote, DOOR_NONE, sizeof d.remote);
            d.sent = 0xFF;
        }
    }
};

Garages g_garages;
struct Reg {
    Reg() { core::RegisterModule(&g_garages); }
} g_reg;
}  // namespace

}  // namespace coop
