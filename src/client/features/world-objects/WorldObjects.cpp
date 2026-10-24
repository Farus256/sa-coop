// Objects of the map (lamp posts, signs, fences, bins, hydrants, trees, boxes, windows) in every game. Each game makes
// them from the same map data, so an object is known by its model and the place its map entry (CDummyObject) holds.
// What a player does to one, the other games repeat: it breaks (CObject::ObjectDamage: smashed, in pieces, its damaged
// model), its window cracks or shatters, or it is knocked loose and moves. The game of the player nearest to it sends
// its pose until it lies still (to the players near it), and the other games' copies follow. A game that meets such an
// object later (it was not streamed in then, or it streamed out and in again) gets it as it was left, at once and
// without breaking it again before the player's eyes (a smashed lamp post is simply gone, a bent sign already bent),
// for half an hour; after that the map puts it back as it always does.
#include <algorithm>
#include <cmath>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "entities/vehicle/VehicleState.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"

namespace coop {

namespace {
using namespace game;

constexpr float kRange = 150.0f;               // objects this far from our player are looked at
constexpr uint64_t kKeepMs = 30 * 60 * 1000;   // a record still applies to objects made again this long
constexpr uint64_t kFreshMs = 3000;             // a change this new is shown happening; an older one is just so
constexpr uint64_t kFollowMs = 1500;           // the other game's moves stop: it lies still (or the message got lost)
constexpr int kMaxSendsPerScan = 16;           // an explosion knocks dozens loose at once
enum : uint8_t { F_MOVED = 1, F_MOVING = 2 };  // not where the map put it; still moving

struct Thing {
    uint16_t model = 0;
    CVector origin;        // where its map entry puts it: the same in both games
    uint8_t stage = 0;     // 0 whole, 1 damaged (its damaged model, a cracked window), 2 broken (smashed, in pieces)
    bool moved = false, moving = false;
    CVector pos, speed, turn;
    Quat rot;
    uint8_t weapon = 0;    // what broke it (CObject::m_nLastWeaponDamage): the same proofs decide in the other game
    bool remote = false;   // the other game's: this one follows
    bool following = false;  // our copy plays the other game's moves
    uint64_t updatedMs = 0, sentMs = 0, appliedMs = 0;
};
std::vector<Thing> g_things;
uint64_t g_scanMs = 0, g_statsMs = 0;
int g_sent = 0, g_received = 0;  // messages since the last stats line

uint8_t ObjectType(CObject* o) { return Field<uint8_t>(o, 0x13C); }  // CObject::m_nObjectType: 1 the map's
CEntity* Dummy(CObject* o) {                                           // CObject::m_pDummyObject: its map entry
    auto* d = Field<CEntity*>(o, 0x170);
    auto* pool = Ref<CPoolHeader*>(0xB744A0);  // CPools::ms_pDummyPool, 0x38 bytes each
    uintptr_t off = (uintptr_t)d - (pool ? (uintptr_t)pool->objects : 0);
    if (!d || !pool || (uintptr_t)d < (uintptr_t)pool->objects || off % 0x38 || off / 0x38 >= (uintptr_t)pool->size ||
        pool->byteMap[off / 0x38] < 0)
        return nullptr;
    return d;
}

// what is the players' to break and move: not what the game moves itself or what explodes in the breaker's name
bool Shared(CObject* o) {
    if (ObjectType(o) != 1 || !o->Matrix() || !Dummy(o)) return false;
    if (Field<void*>(o, 0x138)) return false;  // m_pControlCodeList: garage doors, crossings, cranes, sam sites...
    uint8_t special = Field<uint8_t>(o, 0x145);  // m_nSpecialColResponseCase
    if (special >= 6 && special <= 9) return false;  // swinging and locked doors, hanging things, pool balls
    void* info = Field<void*>(o, 0x160);  // m_pObjectInfo
    return !info || !Field<uint8_t>(info, 0x1F);  // m_bCausesExplosion: CObject::Explode names this game's player
}

uint8_t StageOf(CObject* o) {
    uint32_t f = o->Flags();
    if (!(f & 1) && !(f & 0x80)) return 2;  // no collision and not drawn: smashed, in pieces, shattered
    if ((f & 0x200) || (Field<uint32_t>(o, 0x140) & 0x10)) return 1;  // m_bRenderDamaged, bHasBrokenGlass
    return 0;
}
bool Moving(CObject* o) { return !(o->Flags() & 4); }  // CEntity::m_bIsStatic off: knocked loose

bool Turned(CObject* o, CEntity* d) {
    if (Field<uint32_t>(o, 0x140) & 0x100) return false;  // bIsLampPost: poles and trees sway in the wind (their matrix)
    const CMatrix* m = o->Matrix();
    if (const CMatrix* dm = d->Matrix()) return m->up.Dot(dm->up) < 0.999f || m->forward.Dot(dm->forward) < 0.999f;
    float a = std::remainder(o->GetHeading() - d->GetHeading(), 6.2831853f);
    return m->up.z < 0.999f || std::fabs(a) > 0.045f;
}

Thing* Find(uint16_t model, const CVector& origin) {
    for (Thing& t : g_things)
        if (t.model == model && (t.origin - origin).Length() < 0.1f) return &t;
    return nullptr;
}

void Send(const Thing& t, bool reliable, proto::PlayerMask to = proto::kAllPlayers) {
    Writer w(proto::G_WORLD_OBJ);
    w.U16(t.model);
    w.Vec({t.origin.x, t.origin.y, t.origin.z});
    w.U8(t.stage);
    w.U8((t.moved ? F_MOVED : 0) | (t.moving ? F_MOVING : 0));
    w.U8(t.weapon);
    if (t.moved) {
        w.Vec({t.pos.x, t.pos.y, t.pos.z});
        t.rot.Write(w);
        w.Vec({t.speed.x, t.speed.y, t.speed.z});
        w.Vec({t.turn.x, t.turn.y, t.turn.z});
    }
    Net().SendTo(to, reliable ? proto::CH_RELIABLE : proto::CH_STATE, w);
    ++g_sent;
}

// to a pose: out of the world and back (the sector lists, and the moving list a loose object is on)
void Place(CObject* o, const CVector& pos, const Quat& rot, bool loose, const CVector& speed, const CVector& turn) {
    WorldRemove(o);
    if (bool(Moving(o)) != loose) CallVirtual<4>(o, !loose);  // CObject::SetIsStatic
    rot.ToMatrix(*o->Matrix());
    o->Matrix()->pos = pos;
    CallMethod<0x446F90>(o);  // CEntity::UpdateRW
    CallMethod<0x532B00>(o);  // CEntity::UpdateRwFrame
    ((CPhysical*)o)->MoveSpeed() = loose ? speed : CVector{};
    ((CPhysical*)o)->TurnSpeed() = loose ? turn : CVector{};
    WorldAdd(o);
}

// as the other game broke it: the same damage path shows the same pieces
void Break(CObject* o, uint8_t stage, uint8_t weapon) {
    CVector p = o->GetPosition();
    if (Call<0x46A760, bool>(o)) {  // CGlass::IsObjectGlass: windows break in CGlass, not in ObjectDamage
        if (stage >= 2) Call<0x71BC40>((CEntity*)o, 1000.0f, 0.0f, 0.0f, 0.0f, p.x, p.y, p.z, false);  // WindowRespondsToCollision
        else Field<uint32_t>(o, 0x140) |= 0x10;  // bHasBrokenGlass: cracked
        return;
    }
    CVector up{0, 0, 1};
    for (int i = 0; i < 2 && StageOf(o) < stage; ++i)  // a model that changes first and then smashes takes two
        CallMethod<0x5A0D90>(o, 10000.0f, &p, &up, (CEntity*)nullptr, (int)weapon);  // CObject::ObjectDamage
    if (stage >= 2 && StageOf(o) < 2) o->Flags() &= ~(1u | 0x80u);  // nothing to break into (or proof here): just gone
}

// the state of a thing broken a while ago, put on an object the map has just made again: no pieces flying, no glass
// falling, no second fall of the lamp post
void BreakQuietly(CObject* o, uint8_t stage) {
    if (stage >= 2) {
        o->Flags() &= ~(1u | 0x80u);  // bUsesCollision, bIsVisible: smashed, its pieces long gone
    } else if (Call<0x46A760, bool>(o)) {  // CGlass::IsObjectGlass
        Field<uint32_t>(o, 0x140) |= 0x10;  // bHasBrokenGlass: cracked
    } else {
        o->Flags() |= 0x200;  // m_bRenderDamaged: its damaged model
    }
}

// a copy here in the state the record has (it was made after the change, or the change is the other game's); while
// the other game moves it, our physics carries it on between its poses
void CatchUp(CObject* o, Thing& t, bool driven) {
    if (StageOf(o) < t.stage) {
        if (NowMs() - t.updatedMs > kFreshMs && !driven) BreakQuietly(o, t.stage);
        else Break(o, t.stage, t.weapon);
    }
    if (!t.moved || StageOf(o) >= 2) return;
    if (driven) {
        if (t.appliedMs != t.updatedMs || !t.following) Place(o, t.pos, t.rot, true, t.speed, t.turn);
    } else if (t.following || Moving(o) || (o->GetPosition() - t.pos).Length() > 0.1f) {
        Place(o, t.pos, t.rot, false, {}, {});  // where it lies
    }
    t.appliedMs = t.updatedMs;
    t.following = driven;
}

void Scan() {
    CPlayerPed* me = FindPlayerPed(0);
    CPoolHeader* pool = ObjectPool();
    if (!me || !pool) return;
    CVector mp = me->GetPosition();
    uint64_t now = NowMs();
    int sends = 0;
    for (int i = 0; i < pool->size; ++i) {
        if (pool->byteMap[i] < 0) continue;
        auto* o = (CObject*)(pool->objects + (size_t)i * kObjectPoolElemSize);
        if (!Shared(o)) continue;
        if ((o->GetPosition() - mp).Length2D() > kRange) {
            // made again far off (the map streams objects in farther than we look): as it was left before it is near
            if (g_things.empty() || Moving(o)) continue;
            Thing* far = Find(o->ModelIndex(), Dummy(o)->GetPosition());
            if (far && !far->moving && (StageOf(o) < far->stage || (far->moved && (o->GetPosition() - far->pos).Length() > 0.1f)))
                CatchUp(o, *far, false);
            continue;
        }
        CEntity* d = Dummy(o);
        CVector at = o->GetPosition(), origin = d->GetPosition();
        Thing* t = Find(o->ModelIndex(), origin);
        if (t && t->remote) {
            // the other game moves it right now, or this copy is behind its last word: as there
            bool driven = t->moving && now - t->updatedMs < kFollowMs;
            if (driven || (StageOf(o) <= t->stage && (!Moving(o) || t->following))) {
                CatchUp(o, *t, driven);
                continue;
            }
        } else if (t && !Moving(o) && (StageOf(o) < t->stage || (t->moved && !t->moving && (at - t->pos).Length() > 0.1f))) {
            CatchUp(o, *t, false);  // ours, made again since (streamed out and in): as it was left
            continue;
        }
        uint8_t stage = StageOf(o);
        bool moving = Moving(o) && stage < 2, moved = moving || (at - origin).Length() > 0.05f || Turned(o, d);
        if (stage == 0 && !moved) continue;  // as the map made it
        if (!t) {
            g_things.push_back({});
            t = &g_things.back();
            t->model = o->ModelIndex();
            t->origin = origin;
        }
        bool changed = t->remote || stage != t->stage || moved != t->moved || moving != t->moving ||
                       (moved && !moving && (at - t->pos).Length() > 0.1f);
        // a loose one that hardly moves (rocking on a slope, bobbing in the water: the game never makes it static again)
        // went out ten times a second for as long as it lay there; such goes once a second
        bool stirring = moving && ((at - t->pos).Length() > 0.05f || ((CPhysical*)o)->TurnSpeed().Length() > 0.002f);
        if (!changed && !(moving && now - t->sentMs >= (stirring ? 100u : 1000u))) continue;
        if (sends >= kMaxSendsPerScan) continue;
        ++sends;
        if (t->remote) LOGD("World object %d at %.1f,%.1f: ours now", t->model, origin.x, origin.y);
        t->remote = t->following = false;
        t->stage = stage;
        t->moved = moved;
        t->moving = moving;
        t->pos = at;
        t->rot = Quat::FromMatrix(*o->Matrix());
        t->speed = ((CPhysical*)o)->MoveSpeed();
        t->turn = ((CPhysical*)o)->TurnSpeed();
        t->weapon = Field<uint8_t>(o, 0x148);  // m_nLastWeaponDamage
        t->updatedMs = t->sentMs = now;
        // a change goes to everyone (a game that comes later finds it so); the poses of its flight to those near it
        Send(*t, changed, changed ? proto::kAllPlayers : PlayersNear(at, o->AreaCode(), kRange + 30.0f, false));
        if (changed && !moving) LOGD("World object %d at %.1f,%.1f: stage %d%s", t->model, origin.x, origin.y, stage, moved ? ", moved" : "");
    }
    // records nobody needs any more
    g_things.erase(std::remove_if(g_things.begin(), g_things.end(),
                                  [&](const Thing& t) { return now - t.updatedMs > kKeepMs; }),
                   g_things.end());
}

// Each game has copies of the other players, and those push things too (a mirror car runs a little ahead of the real
// one): the game of the player nearest to it moves it (in steps of 5 m; the host's, then the lower number, when they
// are about as near)
uint8_t Mover(const CVector& at) {
    CPlayerPed* me = FindPlayerPed(0);
    if (!me) return 255;
    uint8_t host = Net().HostId(), best = Net().MyId();
    int bestStep = (int)((me->GetPosition() - at).Length() / 5.0f);
    auto before = [&](uint8_t id, int step) {
        if (step != bestStep) return step < bestStep;
        if ((id == host) != (best == host)) return id == host;
        return id < best;
    };
    for (auto& [id, rp] : Players::Instance().All()) {
        if (!rp.hasState || !Net().Peer(id)) continue;
        int step = (int)((rp.state.pos - at).Length() / 5.0f);
        if (before(id, step)) best = id, bestStep = step;
    }
    return best;
}

void Handle(uint8_t from, Reader& r) {
    uint16_t model = r.U16();
    Vec3 o = r.Vec();
    uint8_t stage = r.U8(), flags = r.U8(), weapon = r.U8();
    Vec3 p{}, s{}, tn{};
    Quat q;
    if (flags & F_MOVED) {
        p = r.Vec();
        q.Read(r);
        s = r.Vec();
        tn = r.Vec();
    }
    if (!r.Ok() || from == Net().MyId() || !core::IsPlayerReady() || stage > 2 || !IsValidModel(model)) return;
    ++g_received;
    CVector origin{o.x, o.y, o.z};
    Thing* t = Find(model, origin);
    if ((flags & F_MOVING) && Mover(origin) != from) return;  // ours or a third game's to move; where it lies still comes
    if (!t) {
        g_things.push_back({});
        t = &g_things.back();
        t->model = model;
        t->origin = origin;
    }
    t->remote = true;
    t->stage = std::max(t->stage, stage);
    t->moved = (flags & F_MOVED) != 0;
    t->moving = (flags & F_MOVING) != 0;
    t->pos = {p.x, p.y, p.z};
    t->rot = q;
    t->speed = {s.x, s.y, s.z};
    t->turn = {tn.x, tn.y, tn.z};
    t->weapon = weapon;
    t->updatedMs = NowMs();
}

class WorldObjects : public Module {
public:
    const char* Name() const override { return "WorldObjects"; }
    void OnInstallHooks() override { Net().SetHandler(proto::G_WORLD_OBJ, &Handle); }
    void OnSessionEnd() override { g_things.clear(); }
    void OnGameStarted() override {  // a new world: what was broken came back
        for (Thing& t : g_things) t.following = false;
    }
    void OnPeerResync(uint8_t id) override {  // its world is new: what the games know goes there again
        for (const Thing& t : g_things)
            if (!t.moving) Send(t, true, proto::Bit(id));
    }
    void OnPostProcess() override {
        if (!Net().Online() || !core::IsPlayerReady() || NowMs() - g_scanMs < 100) return;
        g_scanMs = NowMs();
        Scan();
        if ((g_sent || g_received) && NowMs() - g_statsMs >= 10000) {
            g_statsMs = NowMs();
            LOGI("World objects: %d messages sent, %d received, %zu known", g_sent, g_received, g_things.size());
            g_sent = g_received = 0;
        }
    }
};

WorldObjects g_worldObjects;
struct Reg {
    Reg() { core::RegisterModule(&g_worldObjects); }
} g_reg;
}  // namespace

}  // namespace coop
