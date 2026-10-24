// The partners' characters in the cutscenes, made anew (0.3.8). Up to three of them stand near CJ in every shot of a
// cutscene with him, in the picture: each game works out the places itself from what it shows (the same cutscene, the
// same camera, the same CJ), so nothing waits for the network and the places change on the very frame of a cut.
//
// A shot: the camera's place and direction; a cut is a jump of the camera (more than 1.5 m, or 25 degrees, from one
// frame to the next). At each cut, and whenever a partner has been out of the picture for a second and a half, the
// places around CJ are tried: rings of 1.3, 1.7 and 2.2 m, every 22.5 degrees. A place
//  * is on CJ's floor (the ground there, or CJ's floor where the cutscene set has no collision),
//  * can be seen from CJ (no wall between them) and is not on another person of the scene (0.9 m) or partner (1 m),
//  * is in the picture: head and feet inside the frame, in front of the camera,
//  * hides nobody (a partner nearer the camera than CJ or an actor must not cover them) and is not itself hidden.
// The best one is near CJ, beside him or behind him as the camera sees it, away from the partner placed before, and
// the one it had if it still fits. If no place passes, the checks of covering are dropped; if still none, the partner
// keeps out of that shot. The place goes with CJ: when he walks, the partners walk along at the same offset.
// They stand with CJ's own listening clips of the cutscenes (SceneMocap), else the ped idles; they walk with his walk.
#include "features/story/cutscene-partner/ScenePartner.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "features/story/cutscene-clips/SceneMocap.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"

namespace coop::scene {

namespace {
using namespace game;
using game::script::Cmd;

constexpr uintptr_t kCutsceneObjects = 0xBC3F18, kNumCutsceneObjects = 0xBC3FE4;  // CCutsceneMgr::ms_pCutsceneObjects
constexpr uintptr_t kCameraMatrix = addr::TheCamera + 0x14;                         // CPlaceable::m_matrix
constexpr uintptr_t kFov = 0x8D5038, kAspect = 0xC3EFA4;                            // CDraw::ms_fFOV, ms_fAspectRatio
constexpr uintptr_t kFrameCounter = 0xB7CB4C;
constexpr int kMaxCast = 3;
constexpr float kRings[] = {1.3f, 1.7f, 2.2f};
constexpr int kAngles = 16;
constexpr float kWalkSpeed = 0.6f;          // CJ faster than this (m/s): the partners walk along
constexpr uint64_t kRepickOutMs = 1500;     // out of the picture this long in one shot: a new place

// ---- the camera of the frame, as the game set it this frame (CCamera::Process ran in CGame::Process)
struct Cam {
    CVector pos, right, fwd, up;
    float tanX = 1, tanY = 1;
    bool ok = false;
};
Cam TheCam() {
    Cam c;
    CMatrix* m = Ref<CMatrix*>(kCameraMatrix);
    if (!m) return c;
    c.pos = m->pos, c.right = m->right.Normalized(), c.fwd = m->forward.Normalized(), c.up = m->up.Normalized();
    float fov = Ref<float>(kFov), aspect = Ref<float>(kAspect);
    if (fov < 5.0f || fov > 170.0f) fov = 70.0f;
    if (aspect < 0.5f || aspect > 4.0f) aspect = 4.0f / 3.0f;
    c.tanX = std::tan(fov * 0.5f * 3.14159265f / 180.0f);
    c.tanY = c.tanX / aspect;
    c.ok = true;
    return c;
}
// the point on the screen (0..1 both ways, y down) and its depth along the view
bool Project(const Cam& c, const CVector& p, float& u, float& v, float& depth) {
    CVector d = p - c.pos;
    depth = d.Dot(c.fwd);
    if (depth < 0.5f) return false;
    u = 0.5f + 0.5f * d.Dot(c.right) / (depth * c.tanX);
    v = 0.5f - 0.5f * d.Dot(c.up) / (depth * c.tanY);
    return true;
}
bool Cut(const Cam& a, const Cam& b) {
    return (a.pos - b.pos).Length() > 1.5f || a.fwd.Dot(b.fwd) < 0.906f;  // cos 25 degrees
}

// ---- the scene
CEntity* CutsceneCj() {
    int n = std::clamp(Ref<int32_t>(kNumCutsceneObjects), 0, 50);
    for (int i = 0; i < n; ++i)
        if (CEntity* o = Ref<CEntity*>(kCutsceneObjects + (uintptr_t)i * 4); o && o->ModelIndex() == 1) return o;  // "csplay"
    return nullptr;
}
// the scene's people (not its props): the objects with a ped's model
std::vector<CEntity*> Actors() {
    std::vector<CEntity*> out;
    int n = std::clamp(Ref<int32_t>(kNumCutsceneObjects), 0, 50);
    for (int i = 0; i < n; ++i) {
        CEntity* o = Ref<CEntity*>(kCutsceneObjects + (uintptr_t)i * 4);
        if (o && o->ModelIndex() != 1 && IsPedModel(o->ModelIndex()) && o->IsVisible()) out.push_back(o);
    }
    return out;
}
float Ground(float x, float y, float near, bool& found) {
    found = false;
    float z = Call<0x5696C0, float>(x, y, near + 1.0f, &found, (CEntity**)nullptr);  // FindGroundZFor3DCoord
    if (found && std::fabs(z - near) > 0.6f) found = false;  // another floor: the set's own floor instead
    return found ? z : near;
}

// a person's box on the screen (feet to head, shoulders wide) and its depth
struct Box {
    float u0 = 0, u1 = 0, v0 = 0, v1 = 0, depth = 0;
    bool ok = false;
    float Area() const { return std::max(0.0f, u1 - u0) * std::max(0.0f, v1 - v0); }
};
Box BoxOf(const Cam& c, const CVector& feet) {
    Box b;
    float u, v, d, u2, v2, d2;
    if (!Project(c, feet, u, v, d) || !Project(c, feet + CVector{0, 0, 1.8f}, u2, v2, d2)) return b;
    float half = 0.3f / (d * c.tanX) * 0.5f;
    b.u0 = std::min(u, u2) - half, b.u1 = std::max(u, u2) + half, b.v0 = std::min(v, v2), b.v1 = std::max(v, v2);
    b.depth = d;
    b.ok = true;
    return b;
}
float Overlap(const Box& a, const Box& b) {
    float w = std::min(a.u1, b.u1) - std::max(a.u0, b.u0), h = std::min(a.v1, b.v1) - std::max(a.v0, b.v0);
    return w > 0 && h > 0 ? w * h : 0.0f;
}

// ---- the partners' places
struct Place {
    bool placed = false, hidden = true;
    CVector offset;            // the feet from CJ's position (it follows CJ)
    float heading = 0;
    uint8_t act = 0;
    uint64_t outSince = 0;     // out of the picture since
};
std::map<uint8_t, Place> g_places;
std::vector<uint8_t> g_cast;
Cam g_lastCam;
CVector g_lastCj;
uint64_t g_lastCjMs = 0;
float g_cjSpeed = 0;
CVector g_cjDir{0, 1, 0};
uint32_t g_frame = 0xFFFFFFFF;
bool g_live = false;
int g_shot = 0;
std::map<uint8_t, Spot> g_spots;  // this frame's

void BuildCast() {
    g_cast.clear();
    uint8_t host = Net().HostId();
    if (Net().MyId() != host) g_cast.push_back(Net().MyId());
    for (auto& [id, rp] : Players::Instance().All())
        if (id != host && rp.ped) g_cast.push_back(id);
    std::sort(g_cast.begin(), g_cast.end());
    if ((int)g_cast.size() > kMaxCast) g_cast.resize(kMaxCast);
}

struct Ctx {
    Cam cam;
    CVector cj;
    float floor = 0;
    Box cjBox;
    std::vector<CVector> actorFeet;
    std::vector<Box> actorBoxes;
};

// how good a place is (< 0: not fit); strict: nobody covers anybody
float Score(const Ctx& x, const CVector& feet, const std::vector<CVector>& taken, bool strict, bool lastPlace) {
    for (const CVector& a : x.actorFeet)
        if ((a - feet).Length2D() < 0.9f) return -1;
    for (const CVector& t : taken)
        if ((t - feet).Length2D() < 1.0f) return -1;
    CVector chest = x.cj + CVector{0, 0, 0.4f}, its = feet + CVector{0, 0, 1.4f};
    if (!Call<0x56A490, bool>(&chest, &its, true, true, false, true, false, false, false)) return -1;  // a wall between
    Box b = BoxOf(x.cam, feet);
    float m = strict ? 0.06f : 0.02f;
    if (!b.ok || b.u0 < m || b.u1 > 1 - m || b.v0 < m * 0.6f || b.v1 > 1 - m * 0.6f || b.depth < 1.2f) return -1;
    auto covers = [&](const Box& other) {  // it stands before the other one and hides much of it
        return other.ok && b.depth < other.depth - 0.2f && Overlap(b, other) > 0.3f * other.Area();
    };
    auto hidden = [&](const Box& other) { return other.ok && other.depth < b.depth - 0.2f && Overlap(b, other) > 0.6f * b.Area(); };
    if (strict) {
        if (covers(x.cjBox) || hidden(x.cjBox)) return -1;
        for (const Box& a : x.actorBoxes)
            if (covers(a) || hidden(a)) return -1;
    }
    float r = (feet - CVector{x.cj.x, x.cj.y, feet.z}).Length2D();
    float score = 10.0f - std::fabs(r - 1.4f) * 3.0f;
    float behind = b.depth - x.cjBox.depth;          // beside or a little behind CJ, not before him
    score += behind > -0.3f ? std::min(behind, 1.2f) : behind * 3.0f;
    float side = std::fabs((b.u0 + b.u1) * 0.5f - (x.cjBox.u0 + x.cjBox.u1) * 0.5f);
    score += std::min(side, 0.25f) * 6.0f;           // apart from him on the screen
    for (const CVector& t : taken) score += std::min((t - feet).Length2D(), 3.0f);  // apart from the other partners
    if (lastPlace) score += 3.0f;                    // no jump without need
    return score;
}

bool PickPlace(const Ctx& x, Place& p, const std::vector<CVector>& taken, uint8_t id) {
    auto tryStrict = [&](bool strict) {
        float best = -1;
        CVector bestFeet;
        if (p.placed && !p.hidden) {  // where it was, if that still fits
            CVector feet = x.cj + p.offset;
            float s = Score(x, feet, taken, strict, true);
            if (s > best) best = s, bestFeet = feet;
        }
        for (float r : kRings)
            for (int i = 0; i < kAngles; ++i) {
                float a = 6.2831853f * (i + (id % 2) * 0.5f) / kAngles;
                CVector at{x.cj.x + std::cos(a) * r, x.cj.y + std::sin(a) * r, 0};
                bool found;
                at.z = Ground(at.x, at.y, x.floor, found);
                float s = Score(x, at, taken, strict, false);
                if (s > best) best = s, bestFeet = at;
            }
        if (best < 0) return false;
        p.offset = bestFeet - x.cj;
        // facing CJ's side of the scene: half towards CJ, half towards the camera's side
        CVector toCj = x.cj - bestFeet, toCam = x.cam.pos - bestFeet;
        toCj.z = toCam.z = 0;
        CVector look = toCj.Normalized() * 0.7f + toCam.Normalized() * 0.3f;
        p.heading = std::atan2(-look.x, look.y);
        return true;
    };
    bool ok = tryStrict(true) || tryStrict(false);
    p.placed = true;
    p.hidden = !ok;
    p.outSince = 0;
    return ok;
}

void Update() {
    uint32_t frame = Ref<uint32_t>(kFrameCounter);
    if (frame == g_frame) return;
    g_frame = frame;
    CEntity* cjEnt = CutsceneCj();
    Cam cam = TheCam();
    g_live = cjEnt && cam.ok;
    if (!g_live) {
        g_spots.clear();
        return;
    }
    Ctx x;
    x.cam = cam;
    x.cj = cjEnt->GetPosition();
    bool found;
    x.floor = Ground(x.cj.x, x.cj.y, x.cj.z - 1.0f, found);
    if (!found) x.floor = x.cj.z - 1.0f;  // the cutscene's own sets often have no collision
    x.cjBox = BoxOf(cam, CVector{x.cj.x, x.cj.y, x.floor});
    for (CEntity* a : Actors()) {
        CVector f = a->GetPosition() - CVector{0, 0, 1.0f};
        x.actorFeet.push_back(f);
        x.actorBoxes.push_back(BoxOf(cam, f));
    }
    // CJ walking: how fast and which way
    uint64_t now = NowMs();
    if (g_lastCjMs && now > g_lastCjMs) {
        CVector d = x.cj - g_lastCj;
        d.z = 0;
        float dt = (now - g_lastCjMs) / 1000.0f;
        float speed = d.Length() / std::max(dt, 0.001f);
        g_cjSpeed = g_cjSpeed * 0.8f + std::min(speed, 8.0f) * 0.2f;
        if (d.Length() > 0.01f) g_cjDir = d.Normalized();
    }
    g_lastCj = x.cj, g_lastCjMs = now;
    bool cut = !g_lastCam.ok || Cut(cam, g_lastCam);
    g_lastCam = cam;
    if (cut) ++g_shot;
    BuildCast();
    std::vector<CVector> taken;
    for (size_t i = 0; i < g_cast.size(); ++i) {
        uint8_t id = g_cast[i];
        Place& p = g_places[id];
        bool repick = cut || !p.placed;
        if (!repick && !p.hidden) {  // out of the picture too long in this shot: a place in it
            Box b = BoxOf(cam, x.cj + p.offset);
            bool in = b.ok && b.u1 > 0.02f && b.u0 < 0.98f && b.v1 > 0.02f && b.v0 < 0.98f;
            p.outSince = in ? 0 : (p.outSince ? p.outSince : now);
            repick = p.outSince && now - p.outSince > kRepickOutMs;
        } else if (!repick && p.hidden) {
            repick = now - p.outSince > kRepickOutMs;  // kept out: another try now and then
            if (!p.outSince) p.outSince = now;
        }
        if (repick) {
            bool ok = PickPlace(x, p, taken, id);
            p.act = (uint8_t)(id + g_shot);
            if (!ok) p.outSince = now;
            LOGD("Cutscene shot %d: player %d %s", g_shot, id, ok ? "placed" : "out of it");
        }
        if (!p.hidden) taken.push_back(x.cj + p.offset);
    }
    // the spots of this frame
    g_spots.clear();
    for (uint8_t id : g_cast) {
        const Place& p = g_places[id];
        Spot s;
        s.ground = p.hidden ? CVector{x.cj.x, x.cj.y, x.floor} : x.cj + p.offset;
        s.heading = p.heading;
        s.act = p.act;
        if (p.hidden) s.pose = POSE_HIDDEN;
        else if (g_cjSpeed > kWalkSpeed) s.pose = POSE_WALK, s.heading = std::atan2(-g_cjDir.x, g_cjDir.y);
        else s.pose = POSE_STAND;
        g_spots[id] = s;
    }
}

// ---- holding a ped on its spot
struct Held {
    int handle = -1;
    uint8_t pose = 255, act = 255;
    uint64_t checkMs = 0;
    float heading = 1e9f;
    std::string anim, block;
};
std::map<CPed*, Held> g_held;

void Anim(const Spot& s, std::string& anim, std::string& block) {
    if (s.pose == POSE_WALK) {
        if (MocapWalkCount()) anim = MocapWalk(0), block = kMocapBlock;
        else anim = "walk_player", block = "PED";
        return;
    }
    if (MocapCount() > 0) {
        anim = MocapClip(s.act % MocapCount()), block = kMocapBlock;
        return;
    }
    anim = s.act % 2 ? "idle_chat" : "idle_stance", block = "PED";
}
}  // namespace

void Prepare() {}

void Analyse() { Update(); }
void Follow() { Update(); }

void SetCast(const std::vector<uint8_t>&) {}  // every game knows the cast itself (BuildCast)
void AllowExtras(bool) {}
bool TakeDecision(uint8_t&, Spot&, float&) { return false; }  // each game places the partners itself
void FollowSpot(uint8_t, const Spot&, float) {}

bool HaveShot() {
    Update();
    return g_live && !g_spots.empty();
}

bool CurrentSpot(uint8_t id, Spot& out) {
    Update();
    auto it = g_spots.find(id);
    if (!g_live || it == g_spots.end()) return false;
    out = it->second;
    return true;
}

void ForEachSpot(void (*fn)(uint8_t id, const Spot& s)) {
    for (auto& [id, s] : g_spots) fn(id, s);
}

int ShotNumber() { return g_shot; }
float ShotLeftMs() { return -1; }

const char* Describe(const Spot& s) {
    static char buf[64];
    static const char* names[] = {"stand", "lean", "sit", "steps", "hidden", "walk"};
    snprintf(buf, sizeof buf, "%s at %.1f %.1f %.1f", names[std::min<int>(s.pose, 5)], s.ground.x, s.ground.y, s.ground.z);
    return buf;
}

void Hold(CPed* ped, const Spot& s) {
    if (!ped || !IsPedValid(ped) || ped->InVehicle()) return;
    Held& h = g_held[ped];
    int handle = GetPedRef(ped);
    if (h.handle != handle) {  // taken now
        h = Held{};
        h.handle = handle;
        Cmd(0x0792).I(handle).Run();     // CLEAR_CHAR_TASKS_IMMEDIATELY
        Cmd(0x04D7).I(handle).I(1).Run();  // FREEZE_CHAR_POSITION: the set may have no floor
        Cmd(0x0619).I(handle).I(0).Run();  // SET_CHAR_COLLISION off: it pushes no actor
    }
    CVector at = s.ground + CVector{0, 0, 1.0f};
    if (s.pose == POSE_HIDDEN) {
        ped->Flags() &= ~0x80u;  // bIsVisible
        SetEntityPosition(ped, at);
        h.pose = POSE_HIDDEN;
        return;
    }
    ped->Flags() |= 0x80u;  // bIsVisible: cutscenes hide the players
    ped->PedFlags() = (ped->PedFlags() | 0x3u) & ~0x200u;  // standing (a frozen ped in the air would fall: no animation)
    // lit like CJ: a frozen ped keeps the light of the place it stood before
    if (CEntity* cj = CutsceneCj()) Field<float>(ped, 0x12C) = Field<float>(cj, 0x12C);  // m_fContactSurfaceBrightness
    if ((ped->GetPosition() - at).Length() > 3.0f) Cmd(0x00A1).I(handle).F(at.x).F(at.y).F(at.z - 1.0f).Run();  // SET_CHAR_COORDINATES
    else SetEntityPosition(ped, at);
    float deg = s.heading * 57.29578f;
    if (h.heading > 1e8f || std::fabs(WrapAngle(s.heading - h.heading)) > 0.02f) {  // 1e9: not set yet
        Cmd(0x0173).I(handle).F(deg).Run();  // SET_CHAR_HEADING
        h.heading = s.heading;
    }
    ped->CurrentRotation() = ped->AimingRotation() = s.heading;
    std::string anim, block;
    Anim(s, anim, block);
    uint64_t now = NowMs();
    bool changed = anim != h.anim || block != h.block || s.pose != h.pose;
    if (changed || now - h.checkMs > 1000) {
        h.checkMs = now;
        if (changed || !Cmd(0x0611).I(handle).S(anim.c_str()).Run()) {  // IS_CHAR_PLAYING_ANIM
            Cmd(0x0605).I(handle).S(anim.c_str()).S(block.c_str()).F(4.0f).I(1).I(1).I(1).I(0).I(-1).Run();  // TASK_PLAY_ANIM, looped, in place
            h.anim = anim, h.block = block, h.pose = s.pose;
        }
    }
}

void ReleaseAll() {
    for (auto& [ped, h] : g_held) {
        if (!IsPedValid(ped) || GetPedRef(ped) != h.handle) continue;
        Cmd(0x0792).I(h.handle).Run();     // CLEAR_CHAR_TASKS_IMMEDIATELY
        Cmd(0x04D7).I(h.handle).I(0).Run();  // FREEZE_CHAR_POSITION off
        Cmd(0x0619).I(h.handle).I(1).Run();  // SET_CHAR_COLLISION on
        ped->Flags() |= 0x80u;
    }
    g_held.clear();
    g_places.clear();
    g_spots.clear();
    g_lastCam = Cam{};
    g_lastCjMs = 0;
    g_cjSpeed = 0;
    g_live = false;
}

void DumpShot(const std::string&) {}
void DumpWorld(const std::string&) {}
CVector LastCj() { return g_lastCj; }

}  // namespace coop::scene
