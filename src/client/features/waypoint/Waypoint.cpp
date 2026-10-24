// Map waypoints. Each game sends its player's waypoint (the target set on the pause menu map) and draws the other
// players' ones on its radar and map. They are drawn rather than added as script blips, so they never get into a save.
// Each looks like the game's own waypoint, a black disc with a crosshair on it, the crosshair in its player's colour so
// whose one it is shows at a glance: the host's always red, the others green, yellow, blue and pink by their numbers.
#include <algorithm>
#include <cmath>
#include <map>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/radar-icons/RadarIcons.h"
#include "shared/api/net-session/Session.h"
#include "shared/game/Game.h"
#include "shared/game/Im2D.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"

namespace coop {

namespace {
using namespace game;

constexpr uintptr_t kTargetBlip = addr::MenuManager + 0x2C;  // FrontEndMenuManager.m_nTargetBlipIndex

std::map<uint8_t, CVector2D> g_remote;  // the other players' waypoints
bool g_sentOn = false;
CVector2D g_sent;
uint64_t g_pollMs = 0;

// the blip the map menu placed, while it is still in use
bool LocalWaypoint(CVector2D& out) {
    int h = Ref<int32_t>(kTargetBlip);
    if (!h) return false;
    uintptr_t t = 0xBA86F0 + (h & 0xFFFF) * 0x28;  // CRadar::ms_RadarTrace
    if (!(Ref<uint8_t>(t + 0x25) & 2) || Ref<uint16_t>(t + 0x14) != (uint16_t)(h >> 16)) return false;  // in use, counter
    const CVector& p = Ref<CVector>(t + 0x08);
    out = {p.x, p.y};
    return true;
}

CRGBA WaypointColor(uint8_t id) {
    if (id == Net().HostId()) return {230, 30, 30, 255};  // red
    static const CRGBA kOthers[] = {{40, 210, 60, 255}, {250, 215, 30, 255}, {40, 120, 255, 255}, {255, 100, 200, 255}};
    int order = 0;  // among the players other than the host, by number
    for (auto& [pid, peer] : Net().Peers())
        if (pid != Net().HostId() && pid < id) ++order;
    if (Net().MyId() != Net().HostId() && Net().MyId() < id) ++order;
    return kOthers[order % 4];
}

// the game's own waypoint look: a black disc the size of a blip with a crosshair on it, here in its player's colour
void DrawCrosshair(float x, float y, const CRGBA& c) {
    using namespace im2d;
    float r = std::floor(ScreenHeight() / 448.0f * 8.0f);  // as a blip
    uint32_t col = Argb(255, c.r, c.g, c.b), black = Argb(255, 0, 0, 0);
    Ring(x, y, 0.0f, r, black, black, 32);                  // the disc
    Ring(x, y, r, r + 1.5f, black, Argb(0, 0, 0, 0), 32);    // its edge, smooth
    float t = std::max(1.0f, std::floor(r * 0.16f)), arm = r * 0.78f, gap = r * 0.22f;
    Quad(x - arm, y - t, x - gap, y + t, col);  // the cross with a gap in the middle
    Quad(x + gap, y - t, x + arm, y + t, col);
    Quad(x - t, y - arm, x + t, y - gap, col);
    Quad(x - t, y + gap, x + t, y + arm, col);
    Ring(x, y, r * 0.5f - t * 0.5f, r * 0.5f + t * 0.5f, col, col, 32);  // and a ring through it
}

// after the game's blips, on the radar and on the menu map
void DrawRemoteWaypoints() {
    for (auto& [id, pos] : g_remote) {
        float x, y;
        radar::ToScreen(pos, x, y);  // onto the radar's edge when farther
        DrawCrosshair(x, y, WaypointColor(id));
    }
    im2d::Flush();
}

class Waypoints : public Module {
public:
    const char* Name() const override { return "Waypoints"; }

    void OnInstallHooks() override {
        radar::AddDrawer(&DrawRemoteWaypoints);
        Net().SetHandler(proto::G_WAYPOINT, [](uint8_t from, Reader& r) {
            bool on = r.U8() != 0;
            CVector2D p{r.F32(), r.F32()};
            if (!r.Ok()) return;
            LOGI("Waypoint of player %d: %s %.0f, %.0f", from, on ? "set" : "cleared", p.x, p.y);
            if (on) g_remote[from] = p;
            else g_remote.erase(from);
        });
    }
    void OnSessionStart() override {
        g_remote.clear();
        g_sentOn = false;
    }
    void OnSessionEnd() override { g_remote.clear(); }
    void OnPeerJoin(uint8_t) override { g_sentOn = false; }  // a newcomer gets the current one
    void OnPeerLeave(uint8_t id) override { g_remote.erase(id); }

    // a save keeps its waypoint, so the host's world gave the partner the host's waypoint as his own
    void OnGameStarted() override {
        int& h = Ref<int32_t>(kTargetBlip);
        if (!Net().Online() || Net().IsHost() || !h) return;
        Call<0x587CE0>(h);  // CRadar::ClearBlip
        h = 0;
    }

    void OnPostProcess() override {
        if (!Net().Online() || NowMs() - g_pollMs < 500) return;
        g_pollMs = NowMs();
        CVector2D p;
        bool on = core::IsPlayerReady() && LocalWaypoint(p);
        if (on == g_sentOn && (!on || std::fabs(p.x - g_sent.x) + std::fabs(p.y - g_sent.y) < 1.0f)) return;
        g_sentOn = on;
        g_sent = p;
        Writer w(proto::G_WAYPOINT);
        w.U8(on);
        w.F32(p.x);
        w.F32(p.y);
        Net().Send(proto::CH_RELIABLE, w);
    }

    static Waypoints& Instance() {
        static Waypoints m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&Waypoints::Instance()); }
} g_reg;
}  // namespace

}  // namespace coop
