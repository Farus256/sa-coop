// Trains the games share. A train is its own game's (a random one, a mission's): that game tells the players near it
// where it is (kind, track, distance along the rails, speed), and their games show it as a mission train of the same
// kind kept where the real one is, set every frame, while their player is near it (a train is several vehicles: 16
// players' trains everywhere would fill the vehicle pool). The copies are locked, the driver is in the train's game.
// Random trains come from one game where players are together: the host's, else the lowest-numbered one's that makes
// its own; a game makes its own only while no such player is near (CTrain::DisableRandomTrains). Whoever rides a
// train sits in the cab of its copy (Players). The kind of a train is not kept on it: it is noted when the train is
// made (CTrain::CreateMissionTrain makes the random ones too).
#include <cmath>
#include <map>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "entities/world-share/WorldShare.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

enum : uint8_t { TRAIN_STATE = 1, TRAIN_GONE = 2 };
constexpr uintptr_t kDisableRandomTrains = 0xC38010;  // CTrain::bDisableRandomTrains
constexpr uint16_t kFrontCarriage = 0x8, kClockwise = 0x40;  // CTrain::m_nTrainFlags (+0x5B8)
constexpr int kTrainModels[] = {449, 537, 538, 569, 570, 590};  // tram, freight, streak and their carriages
// A game makes random trains this far from the players before it that do. A game removes its random trains 220 m from
// its camera (CTrain::DoTrainGenerationAndRemoval, 0x863994): a player 220 to 600 m away from the one making them saw none.
constexpr float kOwnRandomFrom = 220.0f;
constexpr float kCopyIn = 450.0f, kCopyOut = 550.0f;  // a copy here while this game's player is this near the train
constexpr float kSendTo = 600.0f;                     // a train's place goes to the players this near it

float& Speed(CVehicle* t) { return Field<float>(t, 0x5A4); }         // CTrain::m_fTrainSpeed
float& RailDistance(CVehicle* t) { return Field<float>(t, 0x5A8); }  // CTrain::m_fCurrentRailDistance
uint16_t Flags(CVehicle* t) { return Field<uint16_t>(t, 0x5B8); }
int8_t Track(CVehicle* t) { return Field<int8_t>(t, 0x5C0); }
CVehicle* NextCarriage(CVehicle* t) { return Field<CVehicle*>(t, 0x5D4); }

// ours
std::map<CVehicle*, uint8_t> g_kinds;  // engines CTrain::CreateMissionTrain made here, and their kind
std::map<CVehicle*, uint16_t> g_ids;   // the ones announced
uint16_t g_nextId = 1;
uint64_t g_sendMs = 0;
bool g_copying = false;                // a copy is being made: not ours

// the other games'
struct Copy {
    CVehicle* engine;
    uint8_t kind;
    float rail, speed;
    uint64_t ms;
    CVector pos;            // where the real one is (as sent)
    float railOffset = 0;   // this game's distance along the rails at the same place minus the sent one
    uint64_t offMs = 0;     // since when the copy is away from the real one's place
};
std::map<uint32_t, Copy> g_copies;  // by owner << 16 | its id
bool g_randomOff = false;           // we switched the random trains off

using CreateTrainFn = void(__cdecl*)(CVector, bool, uint32_t, CVehicle**, CVehicle**, int32_t, int32_t, bool);
CreateTrainFn g_origCreateTrain = nullptr;
void __cdecl HookCreateMissionTrain(CVector pos, bool clockwise, uint32_t kind, CVehicle** first, CVehicle** last, int32_t node,
                                    int32_t track, bool mission) {
    g_origCreateTrain(pos, clockwise, kind, first, last, node, track, mission);
    if (first && *first && !g_copying) g_kinds[*first] = (uint8_t)kind;
}

bool IsEngine(CVehicle* v) { return IsVehicleValid(v) && v->IsTrain() && (Flags(v) & kFrontCarriage); }

// CTrain::CreateMissionTrain without a node takes the nearest node of all four tracks (CTrain::FindClosestTrackNode,
// 0x6F6BD0) and drops the track it is given. Track 3 runs a few metres beside the main line through Los Santos: a copy
// made there went onto the other track, where the owner's rail distance is another place ("Catalyst": the partner saw
// only the tarp). The nearest node of the owner's own track keeps it there.
int NearestNode(int track, const CVector& p) {
    int n = Ref<int32_t>(0xC38014 + (uintptr_t)track * 4);               // NumTrackNodes
    auto* nodes = Ref<const uint8_t*>(0xC38024 + (uintptr_t)track * 4);  // pTrackNodes: x, y, z in 1/8 m, 10 bytes each
    int best = -1;
    float bestD = 1e30f;
    for (int i = 0; nodes && i < n; ++i) {
        auto* xyz = (const int16_t*)(nodes + (size_t)i * 10);
        float dx = xyz[0] / 8.0f - p.x, dy = xyz[1] / 8.0f - p.y, dz = xyz[2] / 8.0f - p.z;
        if (float d = dx * dx + dy * dy + dz * dz; d < bestD) bestD = d, best = i;
    }
    return best;
}

void SendGone(uint16_t id) {
    Writer w(proto::G_TRAIN);
    w.U8(TRAIN_GONE);
    w.U16(id);
    Net().Send(proto::CH_RELIABLE, w);
}

void SendOurs() {
    for (auto it = g_kinds.begin(); it != g_kinds.end();) {
        CVehicle* t = it->first;
        auto id = g_ids.find(t);
        if (!IsEngine(t)) {
            if (id != g_ids.end()) SendGone(id->second), g_ids.erase(id);
            it = g_kinds.erase(it);
            continue;
        }
        if (id == g_ids.end()) id = g_ids.emplace(t, g_nextId++).first;
        CVector p = t->GetPosition();
        Writer w(proto::G_TRAIN);
        w.U8(TRAIN_STATE);
        w.U16(id->second);
        w.U8(it->second);
        w.I8(Track(t));
        w.U8((Flags(t) & kClockwise) ? 1 : 0);
        w.F32(RailDistance(t));
        w.F32(Speed(t));
        w.Vec({p.x, p.y, p.z});
        Net().SendTo(PlayersNear(p, 0, kSendTo, false), proto::CH_STATE, w);
        ++it;
    }
}

// the copy goes with whoever is in it put out first (a vehicle deleted with peds inside leaves them dangling)
void RemoveCopy(const Copy& c) {
    if (!IsEngine(c.engine)) return;
    for (CVehicle* t = c.engine; t; t = NextCarriage(t)) {
        CVector p = t->GetPosition();
        if (CPed* d = t->Driver()) Cmd(0x0362).I(GetPedRef(d)).F(p.x).F(p.y).F(p.z + 3.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
        for (int s = 0; s < 8; ++s)
            if (CPed* q = t->Passengers()[s]) Cmd(0x0362).I(GetPedRef(q)).F(p.x).F(p.y).F(p.z + 3.0f).Run();
        // CTrain::RemoveOneMissionTrain deletes the carriages without CWorld::RemoveReferencesToDeletedObject: a player
        // standing on the copy's roof (a mission's train, deleted and made again by its script) kept standing on freed
        // memory
        Call<0x565510>(t);
    }
    Call<0x6F5DC0>(c.engine);  // CTrain::RemoveOneMissionTrain
}

void RemoveCopies(int owner = -1) {
    for (auto it = g_copies.begin(); it != g_copies.end();)
        if (owner < 0 || (int)(it->first >> 16) == owner) RemoveCopy(it->second), it = g_copies.erase(it);
        else ++it;
}

void HandleTrain(uint8_t from, Reader& r) {
    uint8_t op = r.U8();
    uint16_t id = r.U16();
    if (!r.Ok() || !core::IsPlayerReady()) return;
    uint32_t key = (uint32_t)from << 16 | id;
    auto it = g_copies.find(key);
    if (op == TRAIN_GONE) {
        if (it != g_copies.end()) RemoveCopy(it->second), g_copies.erase(it);
        return;
    }
    uint8_t kind = r.U8();
    int8_t track = r.I8();
    bool clockwise = r.U8() != 0;
    float rail = r.F32(), speed = r.F32();
    Vec3 p = r.Vec();
    if (!r.Ok() || kind > 15 || track < 0 || track > 3) return;  // CTrain's kinds and tracks
    // CTrain::ProcessControl brings the distance back onto the track a length at a time: NaN or a huge one froze it
    if (!std::isfinite(rail) || !std::isfinite(speed) || std::fabs(rail) > 1e6f || std::fabs(speed) > 50.0f) return;
    CPlayerPed* me = FindPlayerPed(0);
    float d = me ? (CVector{p.x, p.y, p.z} - me->GetPosition()).Length() : 1e9f;
    if (it == g_copies.end() && d > kCopyIn) return;  // far from here: no copy (yet)
    if (it != g_copies.end() && (it->second.kind != kind || !IsEngine(it->second.engine))) {
        RemoveCopy(it->second);
        g_copies.erase(it);
        it = g_copies.end();
    }
    if (it == g_copies.end()) {
        CVector at{p.x, p.y, p.z};
        int node = NearestNode(track, at);
        if (node < 0) return;  // no such track here
        // asked and streamed in the background, the copy comes with a later state: six blocking loads at once (the
        // whole streaming queue flushed) were a freeze each time a train came within 300 m
        bool loaded = true;
        for (int model : kTrainModels)
            if (!IsModelLoaded(model)) RequestModel(model), loaded = false;
        if (!loaded) return;
        CVehicle *first = nullptr, *last = nullptr;
        g_copying = true;
        g_origCreateTrain(at, clockwise, kind, &first, &last, node, track, true);
        g_copying = false;
        if (!first) {
            LOGW("Train %d of player %d (kind %d, track %d) could not be copied here", id, from, kind, track);
            return;
        }
        for (CVehicle* t = first; t; t = NextCarriage(t)) t->DoorLock() = 2;  // CARLOCK_LOCKED: driven in its game
        // made at the node nearest to the real one's place: the distance along the rails there is this game's own
        // reckoning of it (a mission's train put on its rails by coordinates, "Catalyst", was elsewhere along them by
        // the sent distance, and the copy was taken away as far from the player at once)
        Copy c{first, kind, rail, speed, NowMs(), CVector{p.x, p.y, p.z}};
        c.railOffset = RailDistance(first) - rail;
        it = g_copies.emplace(key, c).first;
        LOGI("Train %d of player %d (kind %d, track %d) copied, rails %+.1f", id, from, kind, track, c.railOffset);
    }
    it->second.rail = rail;
    it->second.speed = speed;
    it->second.ms = NowMs();
    it->second.pos = CVector{p.x, p.y, p.z};
}

// near a player before it whose game makes random trains, a game makes none: that one makes them for both
void SwitchRandomTrains() {
    bool off = false;
    if (!Net().IsHost()) {
        CPlayerPed* me = FindPlayerPed(0);
        for (auto& [id, rp] : Players::Instance().All())
            if (rp.hasState && (rp.state.flags & OnFootState::F_TRAINS) && MakesBefore(id) && NowMs() - rp.stateRecvMs < 3000 &&
                (rp.state.pos - me->GetPosition()).Length2D() < kOwnRandomFrom) {
                off = true;
                break;
            }
    }
    Share().trains = !off;
    if (off == g_randomOff) return;
    Ref<uint8_t>(kDisableRandomTrains) = off ? 1 : 0;
    g_randomOff = off;
}

class Trains : public Module {
public:
    const char* Name() const override { return "Trains"; }

    void OnInstallHooks() override {
        mem::HookFn(0x6F7550, &HookCreateMissionTrain, &g_origCreateTrain, "CTrain::CreateMissionTrain");
        Net().SetHandler(proto::G_TRAIN, &HandleTrain);
    }
    void OnSessionEnd() override {
        RemoveCopies();
        g_ids.clear();
        if (g_randomOff) Ref<uint8_t>(kDisableRandomTrains) = 0, g_randomOff = false;
        Share().trains = true;
    }
    void OnPeerLeave(uint8_t id) override { RemoveCopies(id); }
    void OnPeerResync(uint8_t id) override { RemoveCopies(id); }  // its game started over
    void OnGameStarted() override {
        g_copies.clear();  // the old world's trains are gone
        g_kinds.clear();
        g_ids.clear();
        g_randomOff = false;
        Share().trains = true;
    }

    void OnPreProcess() override {
        if (!Net().Online() || !core::IsPlayerReady()) return;
        SwitchRandomTrains();
        // where the real train is now: its place and speed as sent, moved on by the time since (a step is 20 ms)
        CPlayerPed* me = FindPlayerPed(0);
        for (auto it = g_copies.begin(); it != g_copies.end();) {
            Copy& c = it->second;
            bool far = (c.pos - me->GetPosition()).Length() > kCopyOut;  // the real one's place, not the copy's
            // a copy that stays away from the real one's place is made again there
            bool astray = IsEngine(c.engine) && (c.engine->GetPosition() - c.pos).Length() > 40.0f;
            c.offMs = astray ? (c.offMs ? c.offMs : NowMs()) : 0;
            if (!IsEngine(c.engine) || NowMs() - c.ms > 3000 || far || (c.offMs && NowMs() - c.offMs > 1500)) {
                if (c.offMs) LOGW("Train copy %u was away from its train: made again", it->first);
                RemoveCopy(c);
                it = g_copies.erase(it);
                continue;
            }
            Speed(c.engine) = c.speed;
            float sign = (Flags(c.engine) & kClockwise) ? 1.0f : -1.0f;  // the speed is along the train's direction
            RailDistance(c.engine) = c.rail + c.railOffset + sign * c.speed * (float)(NowMs() - c.ms) / 20.0f;
            ++it;
        }
    }

    void OnPostProcess() override {
        if (!Net().Online() || Net().Peers().empty() || !core::IsPlayerReady() || NowMs() - g_sendMs < 100) return;
        g_sendMs = NowMs();
        SendOurs();
    }

    static Trains& Instance() {
        static Trains m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Trains::Instance()); }
} g_reg;
}  // namespace

}  // namespace coop
