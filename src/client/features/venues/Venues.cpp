// New places to go into, made with the game's own entrances (CEntryExit): a door on a street leads into one of the
// interiors the game has, and its way out leads back to that door (the game sends the interior's own exit to the door
// last used to come in, CEntryExit::TransitionStarted). In Las Venturas: two underground gambling dens, with their
// icons on the radar and the map. (the bars and shops of a test build in Los Santos are gone.)
//
// The game's map has no door where these go: each is put by an ordinary shop front near the place given here, found
// when the player first comes near (the collision is loaded then): street level from the nearest road node, then the
// nearest spot on the pavement with a tall, flat wall a step ahead, open space behind and no other entrance close.
// Every game finds the same spot from the same map. A save never holds them: they are taken out while the game saves
// and put back after it.
//
// And the ways in and out of every shop and house are quicker: their fades take 0.4 of the time.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/radar-icons/RadarIcons.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;
using game::script::Cmd;

// ---------------------------------------------------------------- the game's entrances
constexpr uintptr_t kEnexPool = 0x96A7D8;    // CEntryExitManager::mp_poolEntryExits (CEntryExit 0x3C bytes)
constexpr uintptr_t kEnexStack = 0x96A720, kEnexStackPosn = 0x96A7C4;
constexpr int kName = 0x00, kRect = 0x08, kExitPos = 0x20, kArea = 0x32, kLink = 0x38;  // m_nFlags at 0x30
CPoolHeader* Pool() { return Ref<CPoolHeader*>(kEnexPool); }
uint8_t* EnexAt(int i) {
    CPoolHeader* p = Pool();
    return p && i >= 0 && i < p->size && p->byteMap[i] >= 0 ? p->objects + (size_t)i * 0x3C : nullptr;
}
CVector EnexCentre(uint8_t* e) {
    float l = Field<float>(e, kRect), b = Field<float>(e, kRect + 4), r = Field<float>(e, kRect + 8), t = Field<float>(e, kRect + 12);
    return {(l + r) * 0.5f, (b + t) * 0.5f, Field<float>(e, 0x18)};
}

// ---------------------------------------------------------------- what goes where
enum Kind : uint8_t { DEN };
struct Interior {
    int8_t area;
    CVector at;
};
const Interior kDen[] = {{12, {1132.95f, -8.68f, 1000.68f}}, {3, {826.89f, 5.51f, 1004.48f}}};  // a casino, the bookies
struct Venue {
    Kind kind;
    CVector near;   // a place in the street it goes by
    int variant;    // which of its kind's interiors
    // found
    enum State { WAIT, SEARCH, PLACED, FAILED } state = WAIT;
    float streetZ = 0;
    int ring = 0;
    CVector door, out;
    float doorAngle = 0, outAngle = 0;
    int index = -1;           // its slot in the entrance pool
    uint8_t* inside = nullptr;  // the interior's own way out, which our door leads to
    char name[8] = {};
};
std::vector<Venue> g_venues = {
    // Las Venturas: the gambling dens
    {DEN, {2470.0f, 2020.0f, 10.8f}, 0},
    {DEN, {1960.0f, 2160.0f, 10.8f}, 1},
};
constexpr float kNearToPlace = 90.0f;  // the collision round a place is loaded with the player this near
constexpr float kRingStep = 1.5f;
constexpr int kRings = 13;  // out to 18 m

const Interior& InteriorOf(const Venue& v) { return kDen[v.variant % 2]; }

// the interior's own way out (the inside half of its door pair): the entrance in that interior by its middle
uint8_t* InsideOf(const Interior& in) {
    CPoolHeader* p = Pool();
    uint8_t* best = nullptr;
    float bestD = 45.0f;
    for (int i = 0; p && i < p->size; ++i) {
        uint8_t* e = EnexAt(i);
        if (!e || (int8_t)Field<uint8_t>(e, kArea) != in.area) continue;
        float d = (Field<CVector>(e, kExitPos) - in.at).Length();
        if (d < bestD) bestD = d, best = e;
    }
    return best;
}

// ---------------------------------------------------------------- finding the spot
struct ColPoint {
    CVector point;
    uint8_t rest[0x2C - 12];
};
// how far along the line to the first wall of the map (buildings only); > len: none
float WallAt(const CVector& from, const CVector& dir, float len) {
    ColPoint cp{};
    CEntity* hit = nullptr;
    CVector to = from + dir * len;
    if (!Call<0x56BA00, bool>(&from, &to, &cp, &hit, true, false, false, false, false, false, false, false))  // ProcessLineOfSight
        return len + 1.0f;
    return (cp.point - from).Length();
}

bool NearOtherEntrance(const CVector& at) {
    CPoolHeader* p = Pool();
    for (int i = 0; p && i < p->size; ++i) {
        uint8_t* e = EnexAt(i);
        if (!e) continue;
        bool ours = false;
        for (const Venue& v : g_venues) ours |= v.index == i;
        if (!ours && (EnexCentre(e) - at).Length2D() < 10.0f && std::fabs(EnexCentre(e).z - at.z) < 6.0f) return true;
    }
    for (const Venue& v : g_venues)
        if (v.state == Venue::PLACED && (v.door - at).Length2D() < 25.0f) return true;
    return false;
}

// one spot: on the ground at street level, a flat tall wall in front, room behind
bool TrySpot(Venue& v, float x, float y) {
    bool found = false;
    float z = Call<0x5696C0, float>(x, y, v.streetZ + 3.0f, &found, (CEntity**)nullptr);  // FindGroundZFor3DCoord
    if (!found || std::fabs(z - v.streetZ) > 2.5f) return false;
    CVector base{x, y, z + 1.0f};
    if (NearOtherEntrance(base)) return false;
    float bestD = 99.0f;
    CVector bestDir;
    for (int k = 0; k < 16; ++k) {
        float a = 6.2831853f * k / 16.0f;
        CVector d{std::cos(a), std::sin(a), 0.0f};
        float w = WallAt(base, d, 3.0f);
        if (w < bestD) bestD = w, bestDir = d;
    }
    if (bestD < 0.6f || bestD > 2.2f) return false;
    CVector side{-bestDir.y, bestDir.x, 0.0f};
    if (WallAt(base + CVector{0, 0, 2.0f}, bestDir, bestD + 1.0f) > bestD + 0.6f) return false;  // tall: no fence
    if (WallAt(base + side * 1.2f, bestDir, bestD + 1.0f) > bestD + 0.5f) return false;          // flat to both sides
    if (WallAt(base - side * 1.2f, bestDir, bestD + 1.0f) > bestD + 0.5f) return false;
    if (WallAt(base, bestDir * -1.0f, 4.0f) < 4.0f) return false;                                  // open behind
    if (WallAt(base, CVector{0, 0, 1}, 2.5f) < 2.5f) return false;                                // nothing low overhead
    // the door a step from the wall, the way out a little farther; the angles as the game's (0: north, to the left)
    v.door = base + bestDir * (bestD - 0.9f) - CVector{0, 0, 1.0f};
    v.out = v.door - bestDir * 1.6f;
    v.doorAngle = std::atan2(-bestDir.x, bestDir.y) * 57.29578f;
    v.outAngle = std::atan2(bestDir.x, -bestDir.y) * 57.29578f;
    return true;
}

void Place(Venue& v);

void Search(Venue& v) {
    if (v.ring >= kRings) {
        v.state = Venue::FAILED;
        LOGW("Venue by %.0f, %.0f: no shop front found near", v.near.x, v.near.y);
        return;
    }
    float r = v.ring * kRingStep;
    int n = v.ring ? (int)std::ceil(6.2831853f * r / kRingStep) : 1;
    for (int k = 0; k < n; ++k) {
        float a = 6.2831853f * k / n + v.ring * 0.37f;
        if (TrySpot(v, v.near.x + std::cos(a) * r, v.near.y + std::sin(a) * r)) {
            Place(v);
            return;
        }
    }
    ++v.ring;
}

// ---------------------------------------------------------------- the door
void Add(Venue& v) {
    const char* name = v.inside ? (const char*)v.inside + kName : "";
    memcpy(v.name, name, 7);
    v.index = Call<0x43FA00, int>(v.door.x, v.door.y, v.door.z, v.doorAngle, 1.6f, 1.6f, 1.0f,  // CEntryExitManager::AddOne
                                  v.out.x, v.out.y, v.out.z, v.outAngle, 0, 0, 0, 0, 24, 0, (const char*)v.name);
    if (uint8_t* e = EnexAt(v.index)) Field<uint8_t*>(e, kLink) = v.inside;
    else v.index = -1;
}

void Place(Venue& v) {
    v.inside = InsideOf(InteriorOf(v));
    if (!v.inside && v.kind == DEN) v.inside = InsideOf(kDen[(v.variant + 1) % 2]);
    if (!v.inside) {
        v.state = Venue::FAILED;
        LOGW("Venue by %.0f, %.0f: its interior (%d) has no way out to link to", v.near.x, v.near.y, InteriorOf(v).area);
        return;
    }
    Add(v);
    if (v.index < 0) {
        v.state = Venue::FAILED;
        return;
    }
    v.state = Venue::PLACED;
    LOGI("Venue %d placed at %.1f, %.1f, %.1f (angle %.0f), into interior %d", (int)v.kind, v.door.x, v.door.y, v.door.z,
         v.doorAngle, (int8_t)Field<uint8_t>(v.inside, kArea));
}

void Remove(Venue& v) {
    if (v.index < 0) return;
    uint8_t* mine = EnexAt(v.index);
    // the interior's way out leads to the door the player last came in by: back to its own street door
    CPoolHeader* p = Pool();
    for (int i = 0; p && mine && i < p->size; ++i)
        if (uint8_t* e = EnexAt(i); e && Field<uint8_t*>(e, kLink) == mine) {
            uint8_t* own = nullptr;
            for (int j = 0; j < p->size && !own; ++j)
                if (uint8_t* c = EnexAt(j); c && c != mine && Field<uint8_t*>(c, kLink) == e) own = c;
            Field<uint8_t*>(e, kLink) = own;
        }
    for (uint32_t k = 0; mine && k < std::min<uint32_t>(4, Ref<uint32_t>(kEnexStackPosn)); ++k)
        if (Ref<uint8_t*>(kEnexStack + k * 4) == mine) Ref<uint8_t*>(kEnexStack + k * 4) = v.inside ? Field<uint8_t*>(v.inside, kLink) : nullptr;
    Call<0x43FD50>(v.index);  // CEntryExitManager::DeleteOne
    v.index = -1;
}

// CEntryExitManager::Save: the entrances of the save are the map's only
using SaveFn = bool(__cdecl*)();
SaveFn g_origSave = nullptr;
bool __cdecl HookEnexSave() {
    std::vector<Venue*> taken;
    for (Venue& v : g_venues)
        if (v.state == Venue::PLACED && v.index >= 0) Remove(v), taken.push_back(&v);
    bool ok = g_origSave();
    for (Venue* v : taken) Add(*v);
    return ok;
}

// ---------------------------------------------------------------- quicker ways in and out
bool g_inTransition = false;
using TransitionFn = bool(__fastcall*)(void*, void*, CPed*);
TransitionFn g_origFinished = nullptr;
bool __fastcall HookTransitionFinished(void* enex, void* edx, CPed* ped) {
    g_inTransition = true;
    bool r = g_origFinished(enex, edx, ped);
    g_inTransition = false;
    return r;
}
using FadeFn = void(__fastcall*)(void*, void*, float, int16_t);
FadeFn g_origFade = nullptr;
void __fastcall HookFade(void* camera, void* edx, float seconds, int16_t dir) {
    if (g_inTransition) seconds *= 0.4f;
    g_origFade(camera, edx, seconds, dir);
}

void ShowIcons() {
    int key = 9000;
    for (const Venue& v : g_venues) {
        ++key;
        if (v.state == Venue::FAILED) {
            radar::RemoveIcon(key);
            continue;
        }
        radar::SetIcon(key, v.state == Venue::PLACED ? v.door : v.near, 25 /* MAFIACASINO */);
    }
}

class Venues : public Module {
public:
    const char* Name() const override { return "Venues"; }
    void OnInstallHooks() override {
        mem::HookFn(0x5D5970, &HookEnexSave, &g_origSave, "CEntryExitManager::Save");
        mem::HookFn(0x4404A0, &HookTransitionFinished, &g_origFinished, "CEntryExit::TransitionFinished");
        mem::HookFn(0x50AC20, &HookFade, &g_origFade, "CCamera::Fade");
    }
    void OnGameShutdown() override {  // the entrance pool goes with the world
        for (Venue& v : g_venues) {
            v.state = Venue::WAIT;
            v.ring = 0;
            v.index = -1;
            v.inside = nullptr;
        }
    }
    void OnGameStarted() override { ShowIcons(); }
    void OnPostProcess() override {
        if (!core::IsPlayerReady()) return;
        CPlayerPed* me = FindPlayerPed(0);
        if (!me || me->AreaCode() != 0) return;
        CVector at = me->GetPosition();
        int budget = 1;  // one ring of one place per frame
        for (Venue& v : g_venues) {
            if (v.state == Venue::PLACED || v.state == Venue::FAILED || budget <= 0) continue;
            if ((v.near - at).Length2D() > kNearToPlace) continue;
            if (v.state == Venue::WAIT) {
                float x = 0, y = 0, z = 0;
                Cmd(0x02C1).F(v.near.x).F(v.near.y).F(v.near.z).Out(&x).Out(&y).Out(&z).Run();  // GET_CLOSEST_CAR_NODE
                v.streetZ = (x || y) && std::fabs(z - v.near.z) < 25.0f ? z : v.near.z;
                v.state = Venue::SEARCH;
            }
            --budget;
            Search(v);
            if (v.state != Venue::SEARCH) ShowIcons();
        }
    }
};

Venues g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

}  // namespace coop
